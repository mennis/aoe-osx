/*
 *  AoEDevice.h
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOEDEVICE_H__
#define __AOEDEVICE_H__

#include <IOKit/IOTypes.h>
#include <IOKit/ata/IOATATypes.h>
#include <IOKit/ata/IOATADevice.h>
#include <IOKit/ata/IOATAController.h>
//#include "IOATAFamily-173.3.1/IOATAController.h"			// JUST FOR TESTING
#include <IOKit/ata/IOATABusCommand.h>

class AOE_DEVICE_NAME : public IOATADevice
{
	OSDeclareDefaultStructors(AOE_DEVICE_NAME)
	
public:
	void uninit(void);

	static AOE_DEVICE_NAME* create_aoe_device(IOATAController* provider, int nShelf, int nSlot);

	/*!@function attach
	 @abstract override of IOService method.
	 */	
	virtual bool attach(IOService* provider );
	virtual bool start(IOService *provider);
	void set_shelf_slot(int nShelf, int nSlot);
	
	// overrides from IOATADevice to provide actual client interface
	
	
	/*!@function executeCommand
	 @abstract Submit IO requests 
	 */ 
	virtual IOReturn executeCommand(IOATACommand* command);
	
	// create and destroy IOATACommands
	/*!@function allocCommand
	 @abstract create command objects for clients.
	 */
	virtual IOATACommand*	allocCommand( void );
	
	/*!@function freeCommand
	 @abstract Clients use this method to dispose of command objects.
	 */
	virtual void freeCommand( IOATACommand* inCommand); 
	
	
protected:
	/*!@function init
	 @abstract used after creating the nub.
	 */
	virtual bool init(IOATAController* provider, ataUnitID unit, ataDeviceType devType);
	/*!@function publishProperties
	 @abstract publish the nub's properties in the device tree.
	 */	virtual void publishProperties( void );
	
	/*!@function publishBusProperties
	 @abstract puts info about this device's bus capability in the device tree.
	 */	virtual void publishBusProperties(void);
	
	/*!@function publishVendorProperties
	 @abstract will be deprecated.
	 */    virtual void publishVendorProperties( void );
	
	
	/*!@function getDeviceID
	 @abstract get the unit id of this drive (0 or 1)
	 */	virtual IOReturn getDeviceID(void);
	
	/*!@function MyATACallback
	 @abstract to be deprecated.
	 */	static void MyATACallback(IOATACommand* command );
	/*!@function processCallback
	 @abstract to be deprecated.
	 */	void processCallback(IOATACommand* command);
	/*!@function swapBytes16
	 @abstract to be deprecated.
	 */    void swapBytes16( UInt8* dataBuffer, IOByteCount length);
	
	UInt8*		m_pIDResponseBuffer;
	int			m_nShelf;
	int			m_nSlot;
};

#endif		// __AOEDEVICE_H__
