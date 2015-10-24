/*
 *  AoEDevice.cpp
 *  AoE
 *
 * This code is mostly based on the concrete subclass of ATADevice: ATADeviceNub
 * IOATAFamily recommends using ATADevice as the base class.
 *
 * It's mainly a nub reference to the protocol service that matches against it. The protocol uses it to communicate with the AoEController
 * There's not all that much here related to AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <IOKit/IOTypes.h>
#include <IOKit/ata/IOATATypes.h>
#include <IOKit/ata/IOATADevice.h>
#include <IOKit/ata/IOATAController.h>
#include <IOKit/ata/IOATADevConfig.h>
#include "IOSyncer.h"
#include "AoEController.h"
#include "../Shared/AoEcommon.h"
#include "AoEDevice.h"
#include "debug.h"

enum
{
	kDoIDDataComplete,
	kDoSetFeatureComplete
};


struct completionInfo
{
	UInt32 whatToDo;
	IOSyncer* sync;
};


#define kIDBufferBytes 512

#define super IOATADevice

OSDefineMetaClassAndStructors(AOE_DEVICE_NAME, IOATADevice )

// static creator function - used by IOATAControllers to create nubs.
AOE_DEVICE_NAME* AOE_DEVICE_NAME::create_aoe_device(IOATAController* provider, int nShelf, int nSlot)
{
	ataUnitID unit;
	ataDeviceType devType;

	debug("AOE_DEVICE_NAME::create_aoe_device\n");

	AOE_DEVICE_NAME*  nub = new AOE_DEVICE_NAME;
	
	// These types are fixed for our AoE device
	unit = kATADevice0DeviceID;
	devType = kATADeviceType;

	if( !nub )
		return 0L;
	
	if( !nub->init( provider, unit, devType) )
	{
		debugError("AOE_DEVICE_NAME failed to initialise\n");

		nub->release();
		return NULL;
	}
	
	nub->m_nShelf = nShelf;
	nub->m_nSlot = nSlot;
	
	return nub;	
}

//---------------------------------------------------------------------------

bool AOE_DEVICE_NAME::init(IOATAController* provider, ataUnitID unit, ataDeviceType devType)
{
	debug("AOE_DEVICE_NAME::init\n");

	if( !super::init( (OSDictionary*) 0L) )
		return false;
	
	_provider = provider;
	_unitNumber = unit;
	_deviceType = devType;
	
	// allocate a buffer for the identify info from the device	
	m_pIDResponseBuffer = (UInt8*) IOMalloc( kIDBufferBytes );
	
	if( !m_pIDResponseBuffer )
		return false;
	
	IOReturn err = kATANoErr;
	
	// issue the identify command so we can get the vendor strings. These will be set when the command returns
	err = getDeviceID();
	
	if( err )
	{
		debugError("AOE_DEVICE_NAME failed identify device %ld\n", (long int) err);
		
		IOFree( m_pIDResponseBuffer, kIDBufferBytes);	
		return false;	
	}

	return true;
}

void AOE_DEVICE_NAME::uninit(void)
{
	debug("AOE_DEVICE_NAME::uninit\n");

	IOFree( m_pIDResponseBuffer, kIDBufferBytes);	
	m_pIDResponseBuffer = 0;
}

//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool AOE_DEVICE_NAME::attach(IOService* provider )
{
	// A quick check that we are attaching to the correct provider
	debug("AOE_DEVICE_NAME::attach\n");
	IOATAController* controller;
	
	controller = OSDynamicCast( IOATAController, provider);

	if( !controller )
	{
		debugError("ATANub: Provider not IOATAController\n");
		return false;
	}

	
	if( !super::attach( provider) )
	{
		debugError("AOE_DEVICE_NAME's super is unable to attach to provider");
		return false;
	}
	
	return true;
}

bool AOE_DEVICE_NAME::start(IOService *provider)
{
	debug("AOE_DEVICE_NAME::start\n");

	return super::start(provider);
}


//---------------------------------------------------------------------------

// create and destroy IOATACommands
//---------------------------------------------------------------------------

IOATACommand* AOE_DEVICE_NAME::allocCommand( void )
{
	debug("AOE_DEVICE_NAME::allocCommand\n");

	return (IOATACommand*) IOATABusCommand64::allocateCmd32();
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void AOE_DEVICE_NAME::freeCommand( IOATACommand* inCommand)
{
	debug("AOE_DEVICE_NAME::freeCommand\n");
	inCommand->release();
}




//---------------------------------------------------------------------------


//---------------------------------------------------------------------------

// Submit IO requests 
IOReturn AOE_DEVICE_NAME::executeCommand(IOATACommand* command)
{
	IOReturn err;
	IOSyncer* mySync;
	IOATABusCommand* cmd;

	mySync = 0L;
	cmd = OSDynamicCast( IOATABusCommand, command);

	debug("AOE_DEVICE_NAME::executeCommand\n");

	if( !cmd )
		return -1;
	
	if( cmd->getCallbackPtr() == 0L)
	{
		// This function may be deprecated in the future. As this is related to IOATAFamily and IOATABusCommand still uses it, we should wait for that to be updated first
		// See: http://lists.apple.com/archives/Darwin-dev/2008/Jan/msg00035.html
		mySync = IOSyncer::create();
		cmd->syncer = mySync;
	}

	err = _provider->executeCommand( this, cmd);

	if( mySync )
	{
		debugError("executeCommand - BLOCKING - wait for SYNC to complete...\n");
		mySync->wait();
		err = cmd->getResult();
		debugError("executeCommand - UNBLOCKING - SYNC to complete...\n");
	}
	
	return err;	
}

 

IOReturn AOE_DEVICE_NAME::getDeviceID( void )
{
	IOMemoryDescriptor* desc;
	OSString* string;
	
	debug("AOE_DEVICE_NAME::getDeviceID\n");

	desc = IOMemoryDescriptor::withAddress((void *) m_pIDResponseBuffer, kIDBufferBytes, kIODirectionIn);
	
	if( !desc )
	{
 		string = OSString::withCString( "failed" );
		setProperty( "Alloc descriptor", (OSObject *)string );
	 	string->release();
	 	return -1;
	}
	
	IOATABusCommand* cmd = (IOATABusCommand*) allocCommand();
	
	if(!cmd)
	{
 		string = OSString::withCString( "failed" );
		setProperty( "Alloc command", (OSObject *)string );
	 	string->release();
		return -1;
	}	

	// tell the bus what to do, what unit and how long to allow
	cmd->setOpcode( kATAFnExecIO);
	cmd->setFlags(mATAFlagIORead);
	cmd->setUnit( _unitNumber  );
	cmd->setTimeoutMS( 30000);
	
	// setup the buffer for the data
	cmd->setBuffer ( desc);
	cmd->setPosition ((IOByteCount) 0);
	cmd->setByteCount ((IOByteCount) kIDBufferBytes);

	// setup the actual taskfile params for the device
	// only two parameters are needed, the device bit for the unit
	// and the actual command for the device to execute
	cmd->setDevice_Head( ((UInt8)_unitNumber) << 4);	
	cmd->setCommand ( kATAcmdDriveIdentify );		
	
	// set up a call back pointer for the command to complete. 
	// the IOATAController only allows async commands
	
	cmd->setCallbackPtr ( (IOATACompletionFunction*) MyATACallback);

	// set the refCon so the callback knows what to do.
	completionInfo* completion = (completionInfo*)IOMalloc(sizeof(completionInfo));
	completion->whatToDo = 	kDoIDDataComplete;
	cmd->refCon = (void*) completion;
	cmd->refCon2 = (void*) this;
	
	desc->prepare(kIODirectionIn);
	
	// tell the bus to exec the command
	return _provider->executeCommand( this, cmd);
}



//---------------------------------------------------------------------------

//---------------------------------------------------------------------------

void AOE_DEVICE_NAME::publishBusProperties( void )
{
	OSString* string;
	//	OSNumber* number;
	
 	// get some bus info
 	debug("AOE_DEVICE_NAME::publishBusProperties\n");
	
 	IOATABusInfo* theInfo = IOATABusInfo::atabusinfo();
 	if( !theInfo )
	{
		debugError("ATANub IOATABusInfo alloc fail\n");
		
 		return;
 	}
 	
 	if(_provider->provideBusInfo( theInfo ))
 	{
 		// blow it off on error
		debugError("ATANub provide info failed\n");
 		theInfo->release();
 		return;	
 	}
 	
 	switch( theInfo->getSocketType() )
 	{
 		case kInternalATASocket:
	 		string = OSString::withCString( kATAInternalSocketString );
			break;
		case kMediaBaySocket:
 			string = OSString::withCString( kATAMediaBaySocketString );
			break;
		case kPCCardSocket:
 			string = OSString::withCString( kATAPCCardSocketString );	
			break;
		case kInternalSATA:
 			string = OSString::withCString( kATAInternalSATAString );	
			break;
		case kSATABay:
 			string = OSString::withCString( kATASATABayString );	
			break;
		case kInternalSATA2:
 			string = OSString::withCString( kATAInternalSATA2 );	
			break;
		case kSATA2Bay:
 			string = OSString::withCString( kATASATA2BayString );	
			break;
 		default:
 			string = OSString::withCString( kATAUnkownSocketString );
			break;
 	}
 	
  	setProperty( kATASocketKey, (OSObject *)string );
 	string->release();
	
	
	// these properties may be published in the future 
	// if conditions warrant
	/*	
	 number = OSNumber::withNumber( theInfo->getPIOModes(), 32 );
	 setProperty( "piomode bitmap", (OSObject *) number);
	 number->release();
	 
	 number = OSNumber::withNumber( theInfo->getDMAModes(), 32 );
	 setProperty( "dmamode bitmap", (OSObject *) number);
	 number->release();
	 
	 number = OSNumber::withNumber( theInfo->getUltraModes(), 32 );
	 setProperty( "ultramode bitmap", (OSObject *) number);
	 number->release();
	 
	 number = OSNumber::withNumber( theInfo->getUnits(), 32 );
	 setProperty( "units on bus", (OSObject *) number);
	 number->release();
	 */
	
	
	// these properties may be published in the future for support of advanced ATA modes.
	/*	setProperty( "DMA supported", theInfo->supportsDMA());
	 setProperty( "48-bit LBA supported", theInfo->supportsExtendedLBA());
	 setProperty( "command overlap supported", theInfo->supportsOverlapped());
	 setProperty( "DMA-Queued supported", theInfo->supportsDMAQueued());
	 */
	
	theInfo->release();
	
}


//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void AOE_DEVICE_NAME::publishProperties( void )
{
	OSString* string;
	
	debug("AOE_DEVICE_NAME::publishProperties\n");

	switch( _deviceType )
	{
		case kATADeviceType:
			string = OSString::withCString( kATATypeATAString );
			debug("NEW DEVICE --type: %s\n", kATATypeATAString);
			break;			
		case kATAPIDeviceType:
			string = OSString::withCString( kATATypeATAPIString );
			debug("NEW DEVICE --type: %s\n", kATATypeATAPIString);
			break;
		default:
			string = OSString::withCString( kATATypeUnknownString );
			debug("NEW DEVICE --type: %s\n", kATATypeUnknownString);
			break;	
	}
	
 	setProperty( kATADevPropertyKey, (OSObject *)string );
 	string->release();

	OSNumber* number = OSNumber::withNumber( _unitNumber, 32 );

	setProperty( kATAUnitNumberKey, (OSObject *) number);
	setProperty( "IOUnit", (OSObject *) number);
 	number->release();
	
	if( _unitNumber == 0 )
		setLocation("0");
	else
		setLocation("1");

	debug("NEW DEVICE -- Unit number: %d\n", _unitNumber);	
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void AOE_DEVICE_NAME::publishVendorProperties(void)
{
	AOE_CONTROLLER_NAME* pController;
	UInt64	NumSectors;
		
	debug("AOE_DEVICE_NAME::publishVendorProperties\n");
	
	pController = OSDynamicCast(AOE_CONTROLLER_NAME, _provider);

	if ( NULL==pController )
	{
		debugError("Provider type does not belong to us\n");
		return;
	}

	// Check which LBA support we have
	if( IOATADevConfig::sDriveSupports48BitLBA( ( const UInt16*) m_pIDResponseBuffer ) )
	{
		UInt32 upperLBA, lowerLBA;
		
		debug("NEW DEVICE -- 48bit LBA\n");

		// Determine the number of sectors
		IOATADevConfig::sDriveExtendedLBASize(   &upperLBA, &lowerLBA, ( const UInt16*) m_pIDResponseBuffer );
		
		NumSectors = ( ((UInt64) upperLBA) << 32) | ((UInt64) lowerLBA );
		
		OSNumber* extendedCapacity = OSNumber::withNumber( NumSectors, 64 );
		setProperty("extended LBA capacity", (OSObject *) extendedCapacity);
		extendedCapacity->release();
		
		pController->SetLBAExtendedSupport(TRUE);
	}
	else
	{
		debug("NEW DEVICE -- 24bit LBA\n");
		
		NumSectors = ( m_pIDResponseBuffer[60 + 1] << 16 ) + ( UInt32 ) m_pIDResponseBuffer[60];
		
		pController->SetLBAExtendedSupport(TRUE);
	}
	
	pController->set_number_sectors(NumSectors);

	// terminate the strings with 0's
	// this changes the identify data, so we MUST do this part last.
	m_pIDResponseBuffer[94] = 0;
	m_pIDResponseBuffer[40] = 0;
	
	// AoE overrides the model number here
	snprintf((char*)&m_pIDResponseBuffer[54], 93-53, "AoE Shelf:%d Slot:%d",  m_nShelf, m_nSlot);

	// Model number runs from byte 54 to 93 inclusive - byte 94 is set to 
	// zero to terminate that string
	OSString* modelNum = OSString::withCString((const char*) &m_pIDResponseBuffer[54]);

	debug("NEW DEVICE -- Model Number: %s\n", &m_pIDResponseBuffer[54]);

	// now that we have made a deep copy of the model string, poke a 0 into byte 54 
	// in order to terminate the fw-vers string which runs from bytes 46 to 53 inclusive.
	m_pIDResponseBuffer[54] = 0;
	
	OSString* firmVers = OSString::withCString((const char*) &m_pIDResponseBuffer[46]);
	
	// serial number runs from byte 20 to byte 39 inclusive and byte 40 has been terminated with a null
	OSString* serial = OSString::withCString( (const char*) &m_pIDResponseBuffer[20]);	
	
 	setProperty( kATAVendorPropertyKey, (OSObject *)modelNum );
 	setProperty( kATARevisionPropertyKey, (OSObject *)firmVers );
 	setProperty( kATASerialNumPropertyKey, (OSObject *)serial );
	
	serial->release();
	modelNum->release();
	firmVers->release();

	debug("NEW DEVICE -- Revision: %s\n", &m_pIDResponseBuffer[46]);
	debug("NEW DEVICE -- Serial number: %s\n", &m_pIDResponseBuffer[20]);
}


//---------------------------------------------------------------------------

//---------------------------------------------------------------------------



void AOE_DEVICE_NAME::MyATACallback(IOATACommand* command )
{
	debug("AOE_DEVICE_NAME::MyATACallback\n");
	
	if( command->getResult() )
	{
		debugError("Command result error = %ld\n",(long int)command->getResult() );
	}
	
	
	AOE_DEVICE_NAME* self = (AOE_DEVICE_NAME*) command->refCon2;
	
	self->processCallback(command);
}

//---------------------------------------------------------------------------

void AOE_DEVICE_NAME::processCallback(IOATACommand* command)
{
	AOE_DEVICE_NAME* self;
	completionInfo* completer;

	debug("AOE_DEVICE_NAME::processCallback\n");
	
	completer = (completionInfo*) command->refCon;
	self = (AOE_DEVICE_NAME*) command->refCon2;

	switch( completer->whatToDo )
	{
		case  kDoIDDataComplete:
		{
			command->getBuffer()->complete( kIODirectionIn );
			
			IOFree(completer, sizeof(completionInfo));
			
			if( command->getResult() )
			{
				IOReturn err = command->getResult();
				debugError("Command failed with err=%d\n", err);
			}
			
			freeCommand(command);
			
#if defined(__BIG_ENDIAN__)
			// The identify device info needs to be byte-swapped on ppc (big-endian) 
			// systems becuase it is data that is produced by the drive, read across a 
			// 16-bit little-endian PCI interface, directly into a big-endian system.
			// Regular data doesn't need to be byte-swapped because it is written and 
			// read from the host and is intrinsically byte-order correct.	
			swapBytes16( m_pIDResponseBuffer, kIDBufferBytes);
#else /* __LITTLE_ENDIAN__ */
			// Swap the strings in the identify data.
			swapBytes16( &m_pIDResponseBuffer[46], 8);   // Firmware revision
			swapBytes16( &m_pIDResponseBuffer[54], 40);  // Model number
			swapBytes16( &m_pIDResponseBuffer[20], 20);  // Serial number
#endif
			// the 512 byte buffer should contain the correctly byte-ordered
			// raw identity info from the device at this point. 
			
			publishProperties();
			publishBusProperties();
			publishVendorProperties();

			// The service could be registered here if we wanted to mount immediately.
			// However, since we need to wait for the config string to be set to our own, we dont register
			// now and let our parent handle the registration.

			//registerService();
			break;
		}
		case kDoSetFeatureComplete:
		{
			// do nothing on set features.			
			completer->sync->signal();
		}
		default:
		{
			break;
		}
	}// end switch	
	
}


//---------------------------------------------------------------------------
// Obviously this code is very inefficient, but it's only called during the ID command
void AOE_DEVICE_NAME::swapBytes16( UInt8* dataBuffer, IOByteCount length)
{
	IOByteCount	i;
	UInt8	c;
	unsigned char* 	firstBytePtr;

	debug("AOE_DEVICE_NAME::swapBytes16\n");

	for (i = 0; i < length; i+=2)
	{
		firstBytePtr = dataBuffer;				// save pointer
		c = *dataBuffer++;						// Save Byte0, point to Byte1
		*firstBytePtr = *dataBuffer;			// Byte0 = Byte1
		*dataBuffer++= c;						// Byte1 = Byte0
	}
}
