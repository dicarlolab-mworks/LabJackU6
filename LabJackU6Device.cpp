/*
 *  LabJack U6 Plugin for MWorks
 *
 *  Created by Mark Histed on 4/21/2010
 *    (based on Nidaq plugin code by Jon Hendry and John Maunsell)
 *
 */

#include <boost/shared_ptr.hpp>
#include "libusb-1.0/libusb.h"
#include "labjackusb.h"
#include "boost/bind.hpp"
#include <MonkeyWorksCore/Component.h>
#include <cstdlib>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include "u6.h"
#include "LabJackU6Device.h"

#define kBufferLength	2048
#define kDIDeadtimeUS	5000			

#define LJU6_LEVERPRESS_FIO 1
#define LJU6_REWARD_FIO     0
#define LJU6_EMPIRICAL_DO_LATENCY_MS 1   // average when plugged into a highspeed hub.  About 8ms otherwise

#define MINSAMPLINGINTERVALUS	

using namespace mw;

/* Notes to self MH 100422
 This is how we do setup and cleanup
 * Constructor [called at plugin load time]
 Sets instant variables
 * core calls attachPhysicalDevice() (in the member function finalize!)
 -> variableSetup()
 * startup()  [called by core; once, I think]
 * startDeviceIO()  [called by core; every trial]
 * stopDeviceIO()   [called by core; every trial]
 * shutdown() [called by core; once, I think]
 * Destructor
 -> detachPhysicalDevice
 
 What we do:
 Constructor [sets up instance variables]
 
 */


// ... just a forward declaration
void *updateChannels(const boost::shared_ptr<LabJackU6Device> &devicePointer);

/* Object functions **********************/

// Constructor for LabJackU6Device.  MW will later call attachToDevice, which is where we finish our initialization
LabJackU6Device::LabJackU6Device(const boost::shared_ptr <Scheduler> &a_scheduler,
                                 const boost::shared_ptr <Variable> _pulseDurationMS,
                                 const boost::shared_ptr <Variable> _pulseOn,
                                 const boost::shared_ptr <Variable> _leverPress)
{
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("LabJackU6Device: constructor");
	}
	scheduler = a_scheduler;
	pulseDurationMS = _pulseDurationMS;
	pulseOn = _pulseOn;
	leverPress = _leverPress;
	deviceIOrunning = false;
    ljHandle = NULL;
    lastLeverPressValue = -1;  // -1 means always report first value
    lastDITransitionTimeUS = 0;
	
	// Lines added by Antonino Casile
	getCapabilities();	
}


// Copy constructor
LabJackU6Device::LabJackU6Device(const LabJackU6Device& copy){
    assert(0); // "Copy constructor should never be called
}

// Destructor
LabJackU6Device::~LabJackU6Device(){ 
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("LabJackU6Device: destructor");
	}
	if (pulseScheduleNode != NULL) {
		// boost::mutex::scoped_lock locker(pulseScheduleNodeLock);
		pulseScheduleNodeLock.lock();
        pulseScheduleNode->cancel();
		pulseScheduleNode->kill();
		pulseScheduleNodeLock.unlock();
    }
	
	if (pollUpdateChannels != NULL) {
		pollUpdateChannelsLock.lock();
        pollUpdateChannels->cancel();
		pollScheduleNode->kill();
		pollUpdateChannelsLock.unlock();
    }
	
	if (ljHandle != NULL) {
		detachPhysicalDevice();
	}
}

// External function for scheduling

void *endPulse(const shared_ptr<LabJackU6Device> &gp){
	shared_ptr <Clock> clock = Clock::instance();			
    // Does not lock, just calls pulseDOLow()
    if (VERBOSE_IO_DEVICE >= 2) {
        mprintf("LabJackU6Device: endPulse callback at %lld us", clock->getCurrentTimeUS());
    }
    
    gp->pulseDOLow();
    return(NULL);
}


void LabJackU6Device::pulseDOHigh(int pulseLengthUS) {
	shared_ptr <Clock> clock = Clock::instance();
    // Takes and releases pulseScheduleNodeLock
    // Takes and releases driver lock
	
	// Set the DO high first
    // boost::mutex::scoped_lock lock(ljU6DriverLock);  //printf("lock DOhigh\n"); fflush(stdout);
	ljU6DriverLock.lock();
	mprintf("LabJackU6Device: setting pulse high %d ms (%lld)", pulseLengthUS / 1000, clock->getCurrentTimeUS());
    MonkeyWorksTime t1 = clock->getCurrentTimeUS();  // to check elapsed time below
    if (ljU6WriteDI(ljHandle, LJU6_REWARD_FIO, 1) == false) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: writing digital output high; device likely to not work from here on");
        return;
    }
    ljU6DriverLock.unlock();      //printf("unlock DOhigh\n"); fflush(stdout);
	
    if (clock->getCurrentTimeUS() - t1 > 4000) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, 
               "LJU6: Writing the DO took longer than 4ms.  Is the device connected to a high-speed hub?  Pulse length is wrong.");
    }
    
	// Schedule endPulse call
    if (pulseLengthUS <= LJU6_EMPIRICAL_DO_LATENCY_MS+1) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "LJU6: requested pulse length %dms too short (<%dms), not doing digital IO", 
               pulseLengthUS, LJU6_EMPIRICAL_DO_LATENCY_MS+1);
    } else {
        // long enough, do it
        // boost::mutex::scoped_lock pLock(pulseScheduleNodeLock);
		pulseScheduleNodeLock.lock();
        shared_ptr<LabJackU6Device> this_one = shared_from_this();
        pulseScheduleNode = scheduler->scheduleMS(std::string(FILELINE ": ") + tag,
												  (pulseLengthUS / 1000.0) - LJU6_EMPIRICAL_DO_LATENCY_MS, 
												  0, 
												  1, 
												  boost::bind(endPulse, this_one),
												  M_DEFAULT_IODEVICE_PRIORITY,
												  M_DEFAULT_IODEVICE_WARN_SLOP_US,
												  M_DEFAULT_IODEVICE_FAIL_SLOP_US,
												  M_MISSED_EXECUTION_DROP);
		pulseScheduleNodeLock.unlock();
		
        MonkeyWorksTime current = clock->getCurrentTimeUS();
        if (VERBOSE_IO_DEVICE) {
            mprintf("LabJackU6Device:  schedule endPulse callback at %lld us (%lld)", current, clock->getCurrentTimeUS());
        }
        highTimeUS = current;
    }
    
}

void LabJackU6Device::pulseDOLow() {
    // Takes and releases driver lock
	shared_ptr <Clock> clock = Clock::instance();    
    MonkeyWorksTime current = clock->getCurrentTimeUS();
	bool retVal;
    
    if (VERBOSE_IO_DEVICE >= 1) {
        mprintf("LabJackU6Device: pulseDOLow at %lld us (pulse %lld us long)", current, current - highTimeUS);
    };
    
    // set the DO low
    // boost::mutex::scoped_lock lock(ljU6DriverLock);
	
	ljU6DriverLock.lock();
	retVal = ljU6WriteDI(ljHandle, LJU6_REWARD_FIO, 0) == false;
	ljU6DriverLock.unlock();
	
    if (retVal == false) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: writing digital output low; device likely to not work from here on");
        return;
    }
}


bool LabJackU6Device::readDI()
// Takes the driver lock and releases it
{
	shared_ptr <Clock> clock = Clock::instance();
    long int state;
	int retVal;
	
	static bool lastState = 0xff;
	
	if (!this->getActive()) {
		return false;
	}
	
    // boost::mutex::scoped_lock lock(ljU6DriverLock);
	
    MonkeyWorksTime st = clock->getCurrentTimeUS();
	
	ljU6DriverLock.lock();
	retVal = ljU6ReadDI(ljHandle, LJU6_LEVERPRESS_FIO, &state);
	
    if (!retVal) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "Error reading DI, stopping IO and returning FALSE");
		
        stopDeviceIO();  // We are seeing USB errors causing this, and the U6 doesn't work anyway, so might as well stop the threads
		ljU6DriverLock.unlock();
        //Debugger();
        return false;
    }
	
    MonkeyWorksTime elT = clock->getCurrentTimeUS()-st;
	
    if (elT > 5000) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "readDI time elapsed is %d us!!", elT);
    }
    
    // software debouncing
	if (state != lastState) {
		if (clock->getCurrentTimeUS() - lastDITransitionTimeUS < kDIDeadtimeUS) {
			state = lastState;				// discard changes during deadtime
			mwarning(M_IODEVICE_MESSAGE_DOMAIN, 
                     "LabJackU6Device: readDI, debounce rejecting new read (last %lld now %lld, diff %lld)", 
                     lastDITransitionTimeUS, 
                     clock->getCurrentTimeUS(),
                     clock->getCurrentTimeUS() - lastDITransitionTimeUS);
		}
		lastState = state;					// record and report the transition
		lastDITransitionTimeUS = clock->getCurrentTimeUS();
	}
	
	ljU6DriverLock.unlock();
    //lock.unlock();  //printf("unlock readDI\n"); fflush(stdout);
	return(state);
}

// External function for scheduling

void *update_lever(const shared_ptr<LabJackU6Device> &gp){
	shared_ptr <Clock> clock = Clock::instance();
    MonkeyWorksTime st = clock->getCurrentTimeUS();
	
	gp->updateSwitch();                 
    
    // MH 100429: only time elapsed in this function is in readDI()  (which is 0.7-1.2ms with a fast hub)
    //fprintf(stderr, "update_lever elapsed %10d us\n", clock->getCurrentTimeUS()-st);
    MonkeyWorksTime elT = clock->getCurrentTimeUS()-st;
    if (elT > 5000) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "update_lever time elapsed is %d us!!", elT);
    }
    
	return(NULL);
}

bool LabJackU6Device::updateSwitch() {	
	
	bool switchValue = readDI();
	
    // Change MW variable value only if switch state is unchanged, or this is the first time through
    if ( (lastLeverPressValue == -1) // -1 means first time through
        || (switchValue != lastLeverPressValue) ) {
        
        leverPress->setValue(Data(switchValue));
        lastLeverPressValue = switchValue;
    }
	return true;
}


/* IODevice virtual calls (made by MWorks) ***********************/
bool LabJackU6Device::attachPhysicalDevice(){  
    // Attach next available device to this object
    // Also, first time configuration for this device.  
    // Opens device; reset if it is dead; and configure IO ports
    // Takes and releases driver lock
	
	attached_device = new IOPhysicalDeviceReference(0, "LabJackU6");
	
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("LabJackU6Device: attachPhysicalDevice");
	}
    
    this->variableSetup();
    // boost::mutex::scoped_lock lock(ljU6DriverLock);  //printf("lock %s\n", "attachPhysicalDevice");
    
    assert(ljHandle==NULL);  // should not try to configure if already open.  If we relax this in the future
    // go through this code and check that we clean up properly
    
    // Opening first found U6 over USB
	// modified by Antonino Casile
    // if( (ljHandle = openUSBConnection(-1)) == NULL) {
	
	ljU6DriverLock.lock();
	ljHandle = LJUSB_OpenDevice(1, 0, U6_PRODUCT_ID);
	ljU6DriverLock.unlock();
	
	if( ljHandle == NULL) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "Error opening LabJack U6.  Is it connected to USB?");
        return false;  // no cleanup needed
    }
    
    setupU6PortsAndRestartIfDead();
    
    if (VERBOSE_IO_DEVICE >= 0) { // i.e. always
        mprintf("LabJackU6Device: attachPhysicalDevice: found LabJackU6");
        // we should print more USB device info here
    }
    
	// Line added by Antonino Casile
	// getCapabilities();
    return true;
}

bool LabJackU6Device::setupU6PortsAndRestartIfDead() {
	int retVal;
    // This is not a pleasant solution, but it works for now
    // takes and releases lock
    
    // boost::mutex::scoped_lock lock(ljU6DriverLock);  
	
    assert(ljHandle != NULL);  // you must have opened before calling this
	
    // Do physical port setup
    if (!ljU6ConfigPorts(ljHandle)) {
        // assume dead
        
		ljU6DriverLock.lock();
        // Force a USB re-enumerate, and reconnect
        merror(M_IODEVICE_MESSAGE_DOMAIN, "LJU6 found dead!!!  Restarting.  bug: should track this down with LabJack Co.");
        libusb_reset_device((libusb_device_handle *)ljHandle);
        // closeUSBConnection(ljHandle);
		LJUSB_CloseDevice(ljHandle);
        //sleep(0.25); // histed: MaunsellMouse1 - 0.1s not enough, 0.2 works, add a little padding
        sleep(2.0); // histed: Testing!
        mwarning(M_IODEVICE_MESSAGE_DOMAIN, "Sleeping for 250ms after restarting LJU6");  
		ljHandle = openUSBConnection(-1);
		ljU6DriverLock.unlock();
		mprintf("After open USB connection");
        if(ljHandle == NULL) {
            merror(M_IODEVICE_MESSAGE_DOMAIN, "Error: could not reopen USB U6 device after reset; U6 will not work now.");
            return false;  // no cleanup needed
        }
		
		// Redo port setup
		ljU6DriverLock.lock();
		retVal = ljU6ConfigPorts(ljHandle);
		ljU6DriverLock.unlock();
		
		mprintf("After configure ports");
		
        if (!retVal) {
            merror(M_IODEVICE_MESSAGE_DOMAIN, "Error: configuring U6 after restart, U6 will not work now\n");
            return false;  // no cleanup needed
        }
    }
	
    return true;
}

bool LabJackU6Device::startup(){
	// Do nothing right now
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("LabJackU6Device: startup");
	}
	return true;
}


bool LabJackU6Device::shutdown(){
	// Do nothing right now
	if (VERBOSE_IO_DEVICE >= 2) {
		mprintf("LabJackU6Device: shutdown");
	}
	return true;
}


bool LabJackU6Device::startDeviceIO(){
    // Start the scheduled IO on the LabJackU6.  This starts a thread that reads the input ports
	
	if (VERBOSE_IO_DEVICE >= 1) {
		mprintf("LabJackU6Device: startDeviceIO");
	}
	if (deviceIOrunning) {
		merror(M_IODEVICE_MESSAGE_DOMAIN,
               "LabJackU6Device startDeviceIO:  startDeviceIO request was made without first stopping IO, aborting");
        return false;
	}
    
    // check hardware and restart if necessary
    setupU6PortsAndRestartIfDead();
	
	setActive(true);
	deviceIOrunning = true;
	
	shared_ptr<LabJackU6Device> this_one = shared_from_this();
	
	/*
	schedule_nodes_lock.lock();
	pollScheduleNode = scheduler->scheduleUS(std::string(FILELINE ": ") + tag,
                                             (MonkeyWorksTime)0, 
                                             LJU6_DITASK_UPDATE_PERIOD_US, 
                                             M_REPEAT_INDEFINITELY, 
                                             boost::bind(update_lever, this_one),
                                             M_DEFAULT_IODEVICE_PRIORITY,
                                             LJU6_DITASK_WARN_SLOP_US,
                                             LJU6_DITASK_FAIL_SLOP_US,                                             
                                             M_MISSED_EXECUTION_DROP);
	//schedule_nodes.push_back(pollScheduleNode);       
	schedule_nodes_lock.unlock();
	 */
	
	// void *update_lever(const shared_ptr<LabJackU6Device> &gp)
	// boost::function<void *()> _functor
	
	// schedule routine that polls channels in order to read/write their value
	schedule_nodes_lock.lock();
	pollUpdateChannels = scheduler->scheduleUS(std::string(FILELINE ": ") + tag,
												 (MonkeyWorksTime)0, 
												 LJU6_CHANS_UPDATE_PERIOD_US, 
												 M_REPEAT_INDEFINITELY, 
												 boost::bind(updateChannels, this_one),
												 M_DEFAULT_IODEVICE_PRIORITY,
												 LJU6_CHANS_WARN_SLOP_US,
												 LJU6_CHANS_FAIL_SLOP_US,                                             
												 M_MISSED_EXECUTION_DROP);
	//schedule_nodes.push_back(pollScheduleNode);       
	schedule_nodes_lock.unlock();
	 
	return true;
}

bool LabJackU6Device::stopDeviceIO(){
	
    // Stop the LabJackU6 collecting data.  This is typically called at the end of each trial.
    
	if (VERBOSE_IO_DEVICE >= 1) {
		mprintf("LabJackU6Device: stopDeviceIO");
	}
	if (!deviceIOrunning) {
        mwarning(M_IODEVICE_MESSAGE_DOMAIN, "stopDeviceIO: already stopped on entry");
		return false;
	}
	
	// stop all the scheduled DI checking (i.e. stop calls to "updateChannel")
	//stopAllScheduleNodes();								// IO device base class method -- this is thread safe
	if (pollScheduleNode != NULL) {
        //merror(M_IODEVICE_MESSAGE_DOMAIN, "Error: pulseDOL
		// boost::mutex::scoped_lock(pollScheduleNodeLock);
		
		pollScheduleNodeLock.lock();
        pollScheduleNode->cancel();
		//pollScheduleNode->kill();  // MH This is not allowed!  This can make both the USB bus unhappy and also leave the lock
		//    in a locked state.  
		//    If you insist on killing a thread that may be talking to the LabJack you should reset the USB bus.
		pollScheduleNodeLock.unlock();
    }
	
	if (pollUpdateChannels != NULL) {
		pollUpdateChannelsLock.lock();
        pollUpdateChannels->cancel();
		//pollScheduleNode->kill();  // MH This is not allowed!  This can make both the USB bus unhappy and also leave the lock
		//    in a locked state.  
		//    If you insist on killing a thread that may be talking to the LabJack you should reset the USB bus.
		pollUpdateChannelsLock.unlock();
    }
	
	setActive(false);
	deviceIOrunning = false;
	return true;
}

/* Factory: create LabJackU6 object */

boost::shared_ptr<mw::Component> LabJackU6DeviceFactory::createObject(std::map<std::string, std::string> parameters,
																	  mw::mwComponentRegistry *reg) {
	
	const char *PULSE_DURATION = "pulse_duration";
	const char *PULSE_ON = "pulse_on";
	const char *LEVER_PRESS = "lever_press";
	
	// here is just a try to print all the parameters passed
	std::map<std::string, std::string>::iterator it;
	int NPar = 0;
	for (it = parameters.begin(); it != parameters.end(); it++) {
		mprintf("PARAMETER #%d -- %s=%s \n", NPar, it->first.c_str(), it->second.c_str());
		NPar++;
	}
	
	REQUIRE_ATTRIBUTES(parameters, PULSE_DURATION);
	
	boost::shared_ptr<mw::Variable> pulse_duration = boost::shared_ptr<mw::Variable>(new mw::ConstantVariable(Data(M_INTEGER, 0)));
	// Why do I need this given that I have a REQUIRE_ATTRIBUTES before?
	if(parameters.find(PULSE_DURATION) != parameters.end()) {
		pulse_duration = reg->getVariable(parameters.find(PULSE_DURATION)->second);	
		/* checkAttribute(pulse_duration, 
		 parameters.find("reference_id")->second, 
		 PULSE_DURATION, 
		 parameters.find(PULSE_DURATION)->second);
		 */
	}
	
	boost::shared_ptr<mw::Variable> pulse_on = boost::shared_ptr<mw::Variable>(new mw::ConstantVariable(Data(M_BOOLEAN, 0)));	
	if(parameters.find(PULSE_ON) != parameters.end()) {
		pulse_on = reg->getVariable(parameters.find(PULSE_ON)->second);	
		/* checkAttribute(pulse_on, 
		 parameters.find("reference_id")->second, 
		 PULSE_ON, 
		 parameters.find(PULSE_ON)->second);
		 */
	}
	
	boost::shared_ptr<mw::Variable> lever_press = boost::shared_ptr<mw::Variable>(new mw::ConstantVariable(Data(M_INTEGER, 0)));	
	if(parameters.find(LEVER_PRESS) != parameters.end()) {
		lever_press = reg->getVariable(parameters.find(LEVER_PRESS)->second);	
		/* checkAttribute(lever_press, 
		 parameters.find("reference_id")->second, 
		 LEVER_PRESS, 
		 parameters.find(LEVER_PRESS)->second);
		 */
	}
	
	boost::shared_ptr <mw::Scheduler> scheduler = mw::Scheduler::instance(true);
	
	boost::shared_ptr <mw::Component> new_daq = 
	boost::shared_ptr<mw::Component>(new LabJackU6Device(scheduler, pulse_duration, pulse_on, lever_press));
	return new_daq;
}	

void LabJackU6Device::variableSetup() {
	shared_ptr<Variable> doReward = this->pulseOn;
	
	weak_ptr<LabJackU6Device> weak_self_ref(getSelfPtr<LabJackU6Device>());
	shared_ptr<VariableNotification> notif(new LabJackU6DeviceOutputNotification(weak_self_ref));
	doReward->addNotification(notif);
	connected = true;	
}

void LabJackU6Device::detachPhysicalDevice() {
	if (VERBOSE_IO_DEVICE >= 1) {
		mprintf("LabJackU6Device: detachPhysicalDevice");
	}
    assert(connected == true); // "Was not connected on entry to detachPhysicalDevice");
	
    // boost::mutex::scoped_lock lock(ljU6DriverLock);
    assert(ljHandle != NULL); // "Device handle is NULL before attempt to disconnect");
    
	ljU6DriverLock.lock();
	
	// Tonino: This was the previous version
    // closeUSBConnection(ljHandle);
	LJUSB_CloseDevice(ljHandle);
	
	// closeUSBConnection(ljHandle);
	
    ljHandle = NULL;
	connected = false;
	
	ljU6DriverLock.unlock();
}

bool LabJackU6Device::ljU6ConfigPorts(HANDLE Handle) {
	// This function is now superseded by IOChannel_LJ6_Digital::initialize()
	
    /// set up IO ports
    /* uint8 sendDataBuff[6]; // recDataBuff[1];
	long retVal;
    uint8 Errorcode=0, ErrorFrame=0;
	 */
	
    /* // setup one to be output, one input, and set output to zero
	 sendDataBuff[0] = 13;       //IOType is BitDirWrite
	 sendDataBuff[1] = (LJU6_LEVERPRESS_FIO & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
	 sendDataBuff[2] = 13;       //IOType is BitDirWrite
	 sendDataBuff[3] = LJU6_REWARD_FIO & 0x0f | (1 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
	 sendDataBuff[4] = 11;             //IOType is BitStateWrite
	 sendDataBuff[5] = (LJU6_REWARD_FIO & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + State (bit 7)
	 
	 if(ehFeedback(Handle, sendDataBuff, 6, &Errorcode, &ErrorFrame, NULL, 0) < 0) {
	 merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- Errorcode = %d", Errorcode);  // note we will get a more informative error on stdout
	 return false;  
	 }
	 */
	
	
	/* sendDataBuff[0] = 13;       //IOType is BitDirWrite
    sendDataBuff[1] = (LJU6_LEVERPRESS_FIO & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
	retVal = ehFeedback(Handle, sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
	if (retVal != 0) {
		merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 1 Errorcode = %d", Errorcode);
		return false;
	}
	
	sendDataBuff[0] = 13;       //IOType is BitDirWrite
    sendDataBuff[1] = LJU6_REWARD_FIO & 0x0f | (1 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
	retVal = ehFeedback(Handle, sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
	if (retVal != 0) {
		merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 2 Errorcode = %d", Errorcode);
		return false;
	}
	
	sendDataBuff[0] = 11;             //IOType is BitStateWrite
    sendDataBuff[1] = (LJU6_REWARD_FIO & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + State (bit 7)
	retVal = ehFeedback(Handle, sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
	if (retVal != 0) {
		merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 3 Errorcode = %d", Errorcode);
		return false;
	} */

	
    /* if(Errorcode) {
	 merror(M_IODEVICE_MESSAGE_DOMAIN, "ehFeedback: error with command, errorcode was %d");
	 return false;
	 }*/
	
    return true;
}


bool LabJackU6Device::ljU6ReadDI(HANDLE Handle, long Channel, long* State) {
    // returns: 1 on success, 0 on error
    // output written to State
    
    uint8 sendDataBuff[4], recDataBuff[1];
    uint8 Errorcode, ErrorFrame;
	
    sendDataBuff[0] = 10;       //IOType is BitStateRead
    sendDataBuff[1] = Channel;  //IONumber
    
    //printf("entering ljU6ReadDI\n"); fflush(stdout);
    if(ehFeedback(Handle, sendDataBuff, 2, &Errorcode, &ErrorFrame, recDataBuff, 1) < 0) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout");  // note we will get a more informative error on stdout
        return false;
    }
    if(Errorcode) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "ehFeedback: error with command, errorcode was %d");
        return false;
    }
    
    *State = (long int)recDataBuff[0];
    return true;
	
}

bool LabJackU6Device::ljU6WriteDI(HANDLE Handle, long Channel, long State) {
	
    uint8 sendDataBuff[2]; // recDataBuff[1];
    uint8 Errorcode, ErrorFrame;
	
    sendDataBuff[0] = 11;             //IOType is BitStateWrite
    sendDataBuff[1] = Channel + 128*((State > 0) ? 1 : 0);  //IONumber(bits 0-4) + State (bit 7)
	
    if(ehFeedback(Handle, sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0) < 0) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout");  // note we will get a more informative error on stdout
        return false;
    }
    if(Errorcode) {
        merror(M_IODEVICE_MESSAGE_DOMAIN, "ehFeedback: error with command, errorcode was %d");
        return false;
    }
    
    return true;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
//                  Code Added by Antonino Casile
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

// This function updates all channels it is scheduled via a timer routine
void *updateChannels(const shared_ptr<LabJackU6Device> &devicePointer) {
	
	// here we call the member function that updates all the channels of the device
	devicePointer->updateAllChannels();
	
	return(NULL);
}

bool LabJackU6Device::updateAllChannels() {
	int i;
	Data data;
	long State;
	IOChannel_LJ6_Digital *chan;
	boost::shared_ptr<Variable> var;
	bool ljResult;
	
	for (i=1; i<channels->getNElements(); i++) {
		chan = (IOChannel_LJ6_Digital *)(channels->getRawElement(i));
		
		if (chan->getDataDirection() == M_INPUT_DATA) {
			// mprintf("--------------------- reading data");
			// we have to set the value of the variable based on the value
			// of the data read out from the LabJack interface
			
			ljU6DriverLock.lock();
			ljResult = ljU6ReadDI(ljHandle, chan->getChannelNum(), &State);
			ljU6DriverLock.unlock();
			
			if (ljResult == false) {
				merror(M_IODEVICE_MESSAGE_DOMAIN, "could not read value of channel %d", i);
				return false;
			}
			
			// now update linked variable ... if any
			if ((var = chan->getVariable()) != NULL) {
				if (State == 1) {
					(*var) = true; 
				} else {
					(*var) = false;
				}
			}
		} else if (chan->getDataDirection() == M_OUTPUT_DATA) {
			//ljU6DriverLock.lock();
			//ljResult = ljU6WriteDI(ljHandle, chan->getChannelNum(), 1);
			//ljU6DriverLock.unlock();
			
			// mprintf("------- var is %d", chan->getVariable()->getValue().getBool());
			
			if (chan->getVariable()->getValue().getBool() == 0) {
			 ljU6DriverLock.lock();
			 ljResult = ljU6WriteDI(ljHandle, chan->getChannelNum(), 0);
			 ljU6DriverLock.unlock();
			 } else {
			 ljU6DriverLock.lock();
			 ljResult = ljU6WriteDI(ljHandle, chan->getChannelNum(), 1);
			 ljU6DriverLock.unlock();
			 
			 }
			
			/*
			var = chan->getVariable();
			if (var != NULL) {
				if (var->getValue().getBool() == 0) {
					ljU6DriverLock.lock();
					ljResult = ljU6WriteDI(ljHandle, chan->getChannelNum(), 0);
					ljU6DriverLock.unlock();
				} else {
					ljU6DriverLock.lock();
					ljResult = ljU6WriteDI(ljHandle, chan->getChannelNum(), 1);
					ljU6DriverLock.unlock();
					
				}
			}
			 */
		}
	}
	
	return true;
}


IOChannel_LJ6_Digital::IOChannel_LJ6_Digital(IOChannelRequest * _request, IOCapability * _capability, 
											 shared_ptr< LabJackU6Device> _device) : IOChannel(_request, _capability) {
	int ind;
	std::string CapabilityName;
	
	// get the name of the capability
	CapabilityName = _request->getRequestedCapabilityName();
	
	// we have found the string now we have to extract the number
	if ( (ind = CapabilityName.find("D_Input")) >= 0) {
		// we have requested the channel as input channel=
		channelNum = atoi(CapabilityName.substr(ind+7, 2).c_str());
		channelType = M_DIGITAL_DATA;
		dataDirection = M_INPUT_DATA;
	} else if ((ind = CapabilityName.find("D_Output")) >= 0) {
		// we have requested the channel as input channel=
		channelNum = atoi(CapabilityName.substr(ind+8, 2).c_str());
		channelType = M_DIGITAL_DATA;
		dataDirection = M_OUTPUT_DATA;
	}
	
	this_device = _device;
}

bool IOChannel_LJ6_Digital::initialize() {
	/// set up IO ports
    uint8 sendDataBuff[2]; // recDataBuff[1];
	long retVal;
    uint8 Errorcode=0, ErrorFrame=0;
	
	mprintf("-------- HANDLE = %p", this_device->getHandle());
	
	if (dataDirection == M_INPUT_DATA) {
		sendDataBuff[0] = 13;       //IOType is BitDirWrite
		sendDataBuff[1] = ((uint8)channelNum & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
		retVal = ehFeedback(this_device->getHandle(), sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
		if (retVal != 0) {
			merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 1 Errorcode = %d", Errorcode);
			return false;
		}
	} else if (dataDirection == M_OUTPUT_DATA) {
		sendDataBuff[0] = 13;       //IOType is BitDirWrite
		sendDataBuff[1] = ((uint8)channelNum & 0x0f) | (1 << 7);  //IONumber(bits 0-4) + Direction (bit 7; 1 is output)
		retVal = ehFeedback(this_device->getHandle(), sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
		if (retVal != 0) {
			merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 1 Errorcode = %d", Errorcode);
			return false;
		}
		
		// we initialize output channels to 0 by default
		sendDataBuff[0] = 11;             //IOType is BitStateWrite
		sendDataBuff[1] = ((uint8)channelNum & 0x0f) | (0 << 7);  //IONumber(bits 0-4) + State (bit 7)
		retVal = ehFeedback(this_device->getHandle(), sendDataBuff, 2, &Errorcode, &ErrorFrame, NULL, 0);
		if (retVal != 0) {
			merror(M_IODEVICE_MESSAGE_DOMAIN, "bug: ehFeedback error, see stdout -- write 3 Errorcode = %d", Errorcode);
			return false;
		}
	}
	
	// now set the channel to initialized
	initialized = true;
	return true;
}

// This function gets the number of digits needed to express the number Num
// in the base Base
unsigned int getNumberDigits(unsigned int Base, unsigned int Num) {
	unsigned int NDigits = 1;
	
	while (1) {
		if (ceil(pow((double)Base, (double)NDigits)) > Num) {
			break;
		} else {
			NDigits++;
		}
	}
	
	return NDigits;
}

// this is to create the new channel each time we assign the capability
IOChannel *LabJackU6Device::makeNewChannel(IOChannelRequest* _request, IOCapability* _capability) {
	int ind, channelNum, capabilityNum;
	std::string CapabilityName;
	std::string TMPCapabilityName;
	unsigned int NDigitsChannels = getNumberDigits(10, N_DIGITAL_CHANNELS);
	unsigned int TMPNDigits;
	char UsedChar = '0';
	
	// get the name of the capability
	CapabilityName = _request->getRequestedCapabilityName();
	
	channelNum = -1;
	
	// get the number of the channel
	if ( (ind = CapabilityName.find("D_Input")) >= 0) {
		// we have requested the channel as input channel=
		channelNum = atoi(CapabilityName.substr(ind+7, 2).c_str());
	} else if ((ind = CapabilityName.find("D_Output")) >= 0) {
		// we have requested the channel as input channel=
		channelNum = atoi(CapabilityName.substr(ind+8, 2).c_str());
	}
	
	if (channelNum == -1) {
		merror(M_IODEVICE_MESSAGE_DOMAIN,
			   "Could not find channelNum in function makeNewChannel()");
	} else {
		
		// and now we set as not available the capabilites that have been assigned
		if (channelNum != -1) {
			TMPNDigits = getNumberDigits(10, channelNum);
			
			// create temporary string used to pad zeros
			std::string buffer(NDigitsChannels-TMPNDigits, UsedChar);
			
			TMPCapabilityName = "LabJack_D_Input" + buffer + (boost::format("%d") % channelNum).str();
			capabilityNum = matchCapabilityByName(TMPCapabilityName);
			
			if (capabilityNum == -1) {
				merror(M_IODEVICE_MESSAGE_DOMAIN,
					   "LabjackU6Device.cpp -- capabilityNum = -1!!! That should not be!!");
			} else {
				mprintf("setting capability %s to NOT available", (*capabilities)[capabilityNum]->getName().c_str());
				(*capabilities)[capabilityNum]->setAvailable(false);
			}
			
			TMPCapabilityName = "LabJack_D_Output" + buffer + (boost::format("%d") % channelNum).str();
			capabilityNum = matchCapabilityByName(TMPCapabilityName);
			
			if (capabilityNum == -1) {
				merror(M_IODEVICE_MESSAGE_DOMAIN,
					   "LabjackU6Device.cpp -- capabilityNum = -1!!! That should not be!!");
			} else {
				mprintf("setting capability %s to NOT available", (*capabilities)[capabilityNum]->getName().c_str());
				(*capabilities)[capabilityNum]->setAvailable(false);
			}
		}
	}
	
	return new IOChannel_LJ6_Digital(_request, _capability, shared_from_this());	
}


// Find out what a specific device can do -- build a list of capabilities
ExpandableList<IOCapability> *LabJackU6Device::getCapabilities(void) {
	
	unsigned int NDigitsChannels = getNumberDigits(10, N_DIGITAL_CHANNELS);
	unsigned int TMPNDigits;
	char UsedChar = '0';
	
	if (!capabilitiesSet) {
		
		// Here I initialize the INPUT channels
		for (int i=0; i<N_DIGITAL_CHANNELS; i++) {
			TMPNDigits = getNumberDigits(10, i);
			
			// create temporary string used to pad zeros
			std::string buffer(NDigitsChannels-TMPNDigits, UsedChar);
			
			std::string name = "LabJack_D_Input" + buffer + (boost::format("%d") % i).str();
			IOCapability *capability(new IOCapability(100+i, name, M_INPUT_DATA, M_DIGITAL_DATA,
													  M_HARDWARE_TIMED_SYNCHRONOUS_IO, LJU6_DITASK_UPDATE_PERIOD_US, 0, 5, 1));
			mprintf("created capability %s", name.c_str());
			
			// and now insert the channel among the capabilities
			registerCapability(capability);
		}
		
		// Here I initialize the OUTPUT channels
		for (int i=0; i<N_DIGITAL_CHANNELS ; i++) {
			TMPNDigits = getNumberDigits(10, i);
			
			// create temporary string used to pad zeros
			std::string buffer(NDigitsChannels-TMPNDigits, UsedChar);
			
			std::string name = "LabJack_D_Output" + buffer + (boost::format("%d") % i).str();
			IOCapability *capability(new IOCapability(100 + N_DIGITAL_CHANNELS + i, name, M_OUTPUT_DATA, M_DIGITAL_DATA,
													  M_HARDWARE_TIMED_SYNCHRONOUS_IO, LJU6_DITASK_UPDATE_PERIOD_US, 0, 3.3, 1));
			mprintf("created capability %s", name.c_str());
			
			// and now insert the channel among the capabilities
			registerCapability(capability);
		}
	}
	
	// we have set the capabilities
	capabilitiesSet = true;
	
	// return a pointer to the list of capabilities (protected data)
	return capabilities;
}



