/*
 *  LabJack U6 Plugin for MWorks
 *
 *  Created by Mark Histed on 4/21/2010
 *    (based on Nidaq plugin code by Jon Hendry and John Maunsell)
 *
 */

#ifndef	_LJU6_DEVICE_H_
#define _LJU6_DEVICE_H_

#include "MonkeyWorksCore/Utilities.h"
#include "MonkeyWorksCore/Plugin.h"
#include "MonkeyWorksCore/IODevice.h"
#include "MonkeyWorksCore/ComponentFactory.h"
#include "MonkeyWorksCore/ComponentRegistry_new.h"
#include "labjackusb.h"

#undef VERBOSE_IO_DEVICE
#define VERBOSE_IO_DEVICE 2  // verbosity level is 0-2, 2 is maximum

// variable are updated every 15ms
#define LJU6_DITASK_UPDATE_PERIOD_US 15000    
#define LJU6_DITASK_WARN_SLOP_US     10000
#define LJU6_DITASK_FAIL_SLOP_US     15000

#define LJU6_CHANS_UPDATE_PERIOD_US 30000    
#define LJU6_CHANS_WARN_SLOP_US     20000
#define LJU6_CHANS_FAIL_SLOP_US     30000

// ----------- lines added by Antonino Casile

// caution that in the Labjack each digital channel can be configured
// either as input or output
#define N_DIGITAL_CHANNELS 20

// ----------- end of lines added by Antonino Casile


using namespace std;

namespace mw {
	
	class LabJackU6Device;
	
	// ----------- lines added by Antonino Casile
	class IOChannel_LJ6_Digital : public IOChannel {
	protected:
		// number of channel
		int channelNum;
		
		// input or output?
		IODataDirection dataDirection;
		
		// type of the channel (digital, analog, etc. etc.)
		IODataType channelType;
		
		// physical device to which this channel belongs
		shared_ptr<LabJackU6Device> this_device;
		
	public:
		virtual bool initialize();
		// this is just a syntactic trick to access the protected member update in IOChannel
		virtual void publicUpdate(Data data) {update(data);};
		
		IOChannel_LJ6_Digital(IOChannelRequest * , IOCapability *, shared_ptr< LabJackU6Device> _device);
		~IOChannel_LJ6_Digital() {};
		
		IODataDirection getDataDirection() {return dataDirection;};
		IODataType getDataType() { return channelType;};
		int getChannelNum() {return channelNum;};
		
	};
	
	// ----------- end of lines added by Antonino Casile
	
	class LabJackU6DeviceOutputNotification;
	
	class LabJackU6Device : public IODevice {
		
	protected:  
		
        bool						connected;
		bool						capabilitiesSet; // indicates whether capabilities were already set
		MonkeyWorksTime				lastDITransitionTimeUS;
		boost::shared_ptr <Scheduler> scheduler;
		shared_ptr<ScheduleTask>	pulseScheduleNode;
		shared_ptr<ScheduleTask>	pollScheduleNode;
		boost::mutex				pulseScheduleNodeLock;				
		boost::mutex				pollScheduleNodeLock;
		
		// Added by Antonino Casile
		shared_ptr<ScheduleTask>	pollUpdateChannels;
		boost::mutex				pollUpdateChannelsLock;
		// end of lines added by Antonino Casile
		
        boost::mutex				ljU6DriverLock;		
        MonkeyWorksTime             highTimeUS;  // Used to compute length of scheduled high/low pulses
		
        HANDLE                      ljHandle;
		
		boost::shared_ptr <Variable> pulseDurationMS;
		boost::shared_ptr <Variable> pulseOn;
		boost::shared_ptr <Variable> leverPress;
		//MonkeyWorksTime update_period;  MH this is now hardcoded, users should not change this
		
		bool active;
		int lastLeverPressValue;	
		boost::mutex active_mutex;
		bool deviceIOrunning;	
		
        // raw hardware functions
        bool ljU6ConfigPorts(HANDLE Handle);
        bool ljU6ReadDI(HANDLE Handle, long Channel, long* State);
        bool ljU6WriteDI(HANDLE Handle, long Channel, long State);
		
	public:
		
		LabJackU6Device(const boost::shared_ptr <Scheduler> &a_scheduler,
						const boost::shared_ptr <Variable> _pulseDurationMS,
						const boost::shared_ptr <Variable> _pulseOn,
						const boost::shared_ptr <Variable> _leverPress);
		
		~LabJackU6Device();
        LabJackU6Device(const LabJackU6Device& copy);
		
		// Find out what a specific device can do -- build a list of capabilities
		virtual ExpandableList<IOCapability> *getCapabilities();
		
		// here we override the standard function in IODevice to take into account
		// specific characteristics of the LabJack
		// virtual bool mapRequestsToChannels();
		virtual IOChannel *makeNewChannel(IOChannelRequest* _request, IOCapability* _capability);
		virtual bool updateAllChannels();
		// end of lines added by Antonino Casile
		
		virtual bool startup();
		virtual bool shutdown();	
        virtual bool attachPhysicalDevice();
        virtual bool startDeviceIO();
        virtual bool stopDeviceIO();		
		
		virtual bool updateSwitch();
        void detachPhysicalDevice();
        void variableSetup();
        bool setupU6PortsAndRestartIfDead();
		
		
		bool readDI();
		void pulseDOHigh(int pulseLengthUS);
		void pulseDOLow();
		
		virtual void dispense(Data data){
			if(getActive()){
				bool doReward = (bool)data;
                
				// Bring DO high for pulseDurationMS ms.
				
				
				if (doReward) {
					//bring DO high
					//mprintf("Yum juice!");
					this->pulseDOHigh(pulseDurationMS->getValue());
				}
			}
		}
		virtual void setActive(bool _active){
			boost::mutex::scoped_lock active_lock(active_mutex);
			active = _active;
		}
		
		virtual bool getActive(){
			boost::mutex::scoped_lock active_lock(active_mutex);
			bool is_active = active;
			return is_active;
		}
		
		// returns the handle to the device in case we have to access it
		HANDLE getHandle() {
			return ljHandle;
		};
		
		shared_ptr<LabJackU6Device> shared_from_this() { return static_pointer_cast<LabJackU6Device>(IODevice::shared_from_this()); }
		
	};
	
	
	class LabJackU6DeviceFactory : public ComponentFactory {
		
		shared_ptr<Component> createObject(std::map<std::string, std::string> parameters,
										   mwComponentRegistry *reg);
	};
	
	
	class LabJackU6DeviceOutputNotification : public VariableNotification {
		
	protected:
		
		weak_ptr<LabJackU6Device> daq;
		
		
	public:
		
		LabJackU6DeviceOutputNotification(weak_ptr<LabJackU6Device> _daq){
			daq = _daq;
		}
		
		virtual void notify(const Data& data, MonkeyWorksTime timeUS){
			
			shared_ptr<LabJackU6Device> shared_daq(daq);
			shared_daq->dispense(data);
			
		}
	};
} // namespace mw

#endif






