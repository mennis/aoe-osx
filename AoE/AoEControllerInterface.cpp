/*
 *  AoEControllerInterface.cpp
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include "AoEService.h"
#include "AoEControllerInterface.h"
#include "AoEController.h"
#include "AoEDevice.h"
#include "../Shared/AoEcommon.h"
#include "debug.h"

__BEGIN_DECLS
#include <net/kpi_interface.h>
#include <sys/kernel_types.h>
#include <sys/kpi_mbuf.h>
__END_DECLS

#define LUN_UPDATE_TIME_MS								(60*1000)
#define DEFAULT_TIME_UNTIL_TARGET_OFFLINE_US			(60*1000*1000)

#define super IOService


/*--------------------------------------------------------------------------------------------------------------------------------------------
 *
 * Some quick notes on how mount/unmounts wrt the AoE "config string":
 *
 * The limitations are that:
 *	- There isn't a way to mount/unmount inside the OS X kernel. It MUST be handled from user space (in our case that is aoed)
 *	- Whenever an AoEController/AoEDevice starts (and registers), the drive will automatically mount.
 *
 * So, things work as follows:
 *
 *	When a drive appears with our matching cstring - the drive attaches and mounts
 *	When a drive appears without our matching cstring - it appears in the ControllerInterface list, but isn't registered.
 *	When a mounted drive is unclaimed - it is unmounted, and the driver unattaches drive
 *	When an unmounted drive is claimed: If it was registered already, it mounts as normal
 *										It is registered if it wasn't already. Mounting automatically.
 *	When a mounted drive no longer has it's matching cstring (claimed by another device) - Driver unattaches drive
 *
 *	Upon shutdown, the OS will unmount all drives and the kext will unload as normal.
 *
 *  Caveats
 *		- When a device goes offline, it will be removed from the system (to avoid hanging the finder), this triggers an OS warning
 *		  The only way this could be avoided is to unmount the drive prior to removing it, but this would have to be handled in user space
 *		  with a daemon.
 *
 -------------------------------------------------------------------------------------------------------------------------------------------*/


#pragma mark -
#pragma mark Set up/down

OSDefineMetaClassAndStructors(AOE_CONTROLLER_INTERFACE_NAME, IOService);


bool AOE_CONTROLLER_INTERFACE_NAME::init(AOE_KEXT_NAME* pAoEService)
{
	debug("AOE_CONTROLLER_INTERFACE_NAME::init\n");
	bool nRet = super::init(NULL);
	
	m_pAoEService = pAoEService;
	m_fLUNSearchRunning = FALSE;
	m_pTargetListMutex = IOLockAlloc();
	m_TimeUntilTargetOffline_us = DEFAULT_TIME_UNTIL_TARGET_OFFLINE_US;
	m_pControllerToFakeResponse = NULL;
	m_nCurrentTag = MIN_TAG;
	m_nMaxTransferSize = DEFAULT_MAX_TRANSFER_SIZE;
	
	m_pControllers = OSArray::withCapacity(2);
	
	if ( NULL==m_pControllers )
	{
		debugError("Cannot initialise m_pControllers\n");
		nRet = FALSE;
		goto Done;
	}
	
	// Setup timer to check status periodically
	IOWorkLoop* pWorkLoop = pAoEService->getWorkLoop();
	
	if ( pWorkLoop )
	{
		m_pStateUpdateTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &AOE_CONTROLLER_INTERFACE_NAME::StateUpdateTimer);
		
		if ( !m_pStateUpdateTimer )
		{
			debugError("Unable to create m_pStateUpdateTimer timerEventSource\n");
			nRet = FALSE;
			goto Done;
		}
		
		if ( pWorkLoop->addEventSource(m_pStateUpdateTimer) != kIOReturnSuccess )
		{
			debugError("Unable to add m_pStateUpdateTimer timerEventSource to work loop\n");
			nRet = FALSE;
			goto Done;
		}
		
		m_pFakeReturnTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &AOE_CONTROLLER_INTERFACE_NAME::FakeReturnTimer);
		
		if ( !m_pFakeReturnTimer )
		{
			debugError("Unable to create m_pFakeReturnTimer timerEventSource\n");
			nRet = FALSE;
			goto Done;
		}
		
		if ( pWorkLoop->addEventSource(m_pFakeReturnTimer) != kIOReturnSuccess )
		{
			debugError("Unable to add m_pFakeReturnTimer timerEventSource to work loop\n");
			nRet = FALSE;
			goto Done;
		}
	}
	else
		debugError("Unable to find work loop\n");
	
Done:
	return nRet;
}





void AOE_CONTROLLER_INTERFACE_NAME::uninit(void)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	IOWorkLoop* pWorkLoop;
	
	debug("AOE_CONTROLLER_INTERFACE_NAME::uninit\n");
	
	pWorkLoop = m_pAoEService->getWorkLoop();
	
	// Stop and clean up our timer
	if ( m_pStateUpdateTimer )
	{
		m_pStateUpdateTimer->cancelTimeout();
		if ( pWorkLoop )
		{
			pWorkLoop->removeEventSource(m_pStateUpdateTimer);
			CLEAN_RELEASE(m_pStateUpdateTimer);
		}
	}
	
	if ( m_pFakeReturnTimer )
	{
		m_pFakeReturnTimer->cancelTimeout();
		if ( pWorkLoop )
		{
			pWorkLoop->removeEventSource(m_pFakeReturnTimer);
			CLEAN_RELEASE(m_pFakeReturnTimer);
		}
	}
	
    pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
		{
			pController->uninit();
			pController->terminate();
		}
		
		pControllerIterator->release();
	}
	
	m_pControllers->flushCollection();
	CLEAN_RELEASE(m_pControllers);
	IOLockFree(m_pTargetListMutex);
	m_pTargetListMutex = NULL;
}




#pragma mark -
#pragma mark Tag handling

/*---------------------------------------------------------------------------
 * All outgoing aoe packets will get their tag from here
 ---------------------------------------------------------------------------*/

UInt32	AOE_CONTROLLER_INTERFACE_NAME::next_tag()
{
	++m_nCurrentTag;
	m_nCurrentTag = MAX(m_nCurrentTag, MIN_TAG);
	m_nCurrentTag = MIN(m_nCurrentTag, MAX_TAG);
	
	// Check for wrap around
	if ( m_nCurrentTag==MAX_TAG )
		m_nCurrentTag = MIN_TAG;
	
	return m_nCurrentTag;
}





#pragma mark -
#pragma mark AoE searching

/*---------------------------------------------------------------------------
 * Called in a timer, this function will search all interfaces for connected LUNs
 ---------------------------------------------------------------------------*/

errno_t AOE_CONTROLLER_INTERFACE_NAME::aoe_search(ifnet_t ifnet)
{
	struct ether_header* eh;
	aoe_cfghdr_full* pAoEFullHeader;
	aoe_header*	pAoEHeader;
	aoe_cfghdr* pAoECfg;
	UInt32		Tag;
	size_t Len;
	errno_t	result;
	mbuf_t	m;
	
	debug("aoe_search.......................................................\n");
	
	// Create our mbuf
	result = mbuf_gethdr(MBUF_WAITOK, MBUF_TYPE_DATA, &m);
	if (result != 0)
		return result;
	
	mbuf_setlen(m, sizeof(*pAoEFullHeader));
	mbuf_pkthdr_setlen(m, sizeof(*pAoEFullHeader));
	
	mbuf_align_32(m, sizeof(*pAoEFullHeader));
	pAoEFullHeader = MTOD(m, aoe_cfghdr_full*);
	
	// Add shortcuts
	pAoEHeader = &(pAoEFullHeader->aoe);
	pAoECfg = &(pAoEFullHeader->cfg);
	
	/*
	 * Prepend the ethernet header, we will send the raw frame;
	 * callee frees the original mbuf when allocation fails.
	 */
	result = mbuf_prepend(&m, sizeof(*eh), MBUF_WAITOK);
	if (result != 0)
		return result;
	
	eh = MTOD(m, struct ether_header*);
	eh->ether_type = htons(ETHERTYPE_AOE);
	
	// Fill out the config header
	AOE_HEADER_CLEAR(pAoEHeader);
	pAoEHeader->ah_verflagserr = AOE_HEADER_SETVERFLAGERR(AOE_SUPPORTED_VER, 0, 0);
	pAoEHeader->ah_major = AOE_HEADER_SETMAJOR(SHELF_BROADCAST);
	pAoEHeader->ah_minorcmd = AOE_HEADER_SETMINORCMD(SLOT_BROADCAST, AOE_CFG_COMMAND);
	Tag = TAG_BROADCAST_MASK | next_tag();			// Flag this as broadcast so we dont drop the packets returned by multiple targets (since they will return with the same tag number)
	pAoEHeader->ah_tag[0] = AOE_HEADER_SETTAG1(Tag);
	pAoEHeader->ah_tag[1] = AOE_HEADER_SETTAG2(Tag);
	
	AOE_CFGHEADER_CLEAR(pAoECfg);
	pAoECfg->ac_scnt_aoe_ccmd = AOE_HEADER_SETSECTOR_CMD(m_pAoEService->get_sector_count(), CONFIG_STR_GET);
	
	// Use the broadcast address for this command
	ifnet_llbroadcast_copy_bytes(ifnet, eh->ether_dhost, sizeof(eh->ether_dhost), &Len);
	if ( Len!=sizeof(eh->ether_dhost) )
		debugError("unexpected broadcast address size\n");
	
	return m_pAoEService->send_packet_on_interface(ifnet, Tag, m, -1, FALSE);
}





#pragma mark -
#pragma mark config command parsing

/*---------------------------------------------------------------------------
 * Handle the reception of a received ata packet
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::aoe_ata_receive(aoe_header* pAoEFullHeader, aoe_atahdr_rd* pATAHeader, mbuf_t* pMBufData)
{
	bool fFoundDevice;
	int nMajor;
	int nMinor;
	
	nMajor = AOE_HEADER_GETMAJOR(pAoEFullHeader);
	nMinor = AOE_HEADER_GETMINOR(pAoEFullHeader);
	
	//---------------------------------------------------------//
	// Send the ATA command back to the appropriate Controller //
	//---------------------------------------------------------//
	
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	fFoundDevice = FALSE;
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		// Iterate through list, looking for our ID
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			if ( 0==pController->is_device(nMajor, nMinor) )
			{
				fFoundDevice = TRUE;
				debugVerbose("ATA command received for device %d.%d\n", nMajor, nMinor);
				
				// send command on to our device
				pController->ata_response(pATAHeader, pMBufData, AOE_HEADER_GETTAG(pAoEFullHeader));
			}
		
		pControllerIterator->release();
	}
	else
	{
		debugError("Unable to iterate through controller list\n");
	}
	
	// err if we didn't find the device in our list
	if ( !fFoundDevice )
		debugError("Received an ATA command from a device that wasn't registered\n");
	
	return 0;
}





/*---------------------------------------------------------------------------
 * Handle the reception of a received aoe packet
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::aoe_config_receive(ifnet_t ifnet_receive, struct ether_header* pEHeader, aoe_header* pAoEFullHeader, aoe_cfghdr_rd* pCfgHeader, mbuf_t* pMBufData)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	bool fFoundDevice;
	int nMajor;
	int nMinor;
	
	nMajor = AOE_HEADER_GETMAJOR(pAoEFullHeader);
	nMinor = AOE_HEADER_GETMINOR(pAoEFullHeader);
	
	debug("AOE_CFG_COMMAND - Buf count=%#x Firmware=%x Sector=%#x AoE=%#x CCmd=%#x Length=%#x\n",
		  AOE_CFGHEADER_GETBCOUNT(pCfgHeader),
		  AOE_CFGHEADER_GETFVERSION(pCfgHeader),
		  AOE_CFGHEADER_GETSCOUNT(pCfgHeader),
		  AOE_CFGHEADER_GETAOEVER(pCfgHeader),
		  AOE_CFGHEADER_GETCCMD(pCfgHeader),
		  AOE_CFGHEADER_GETCSLEN(pCfgHeader));
	
	//-------------------------------------------------------------------------------//
	// Send the command back to the appropriate Controller							 //
	//																				 //
	// (NOTE: this is based on the Shelf/Slot number rather than the MAC address)	 //
	//-------------------------------------------------------------------------------//
	fFoundDevice = FALSE;
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		// Iterate through list, looking for our ID
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			if ( 0==pController->is_device(nMajor, nMinor) )
			{
				fFoundDevice = TRUE;
				debugVerbose("AoE cmd received for device %d.%d\n", nMajor, nMinor);
				
				// Update with info
				pController->handle_aoe_cmd(ifnet_receive, pCfgHeader, pMBufData);
				pController->update_target_info(ifnet_receive, pEHeader->ether_shost, TRUE);
				
				// Remove the target if the device no longer belongs to us
				if ( 0 != pController->cstring_is_ours(m_pAoEService->get_com_cstring()) )
				{
					if ( 0 == pController->is_registered() )
					{
						debugWarn("We have lost our device, removing target\n");
						remove_target(pController->target_number());
						
						// Return now in case the iterator is invalid
						pControllerIterator->release();
						return 0;
					}
				}
			}
		
		pControllerIterator->release();
	}
	else
	{
		debugError("Unable to iterate through controller list\n");
	}
	
	//------------------------------------------------------//
	// If we didn't find the device in the list, add it now //
	//------------------------------------------------------//
	
	if ( !fFoundDevice )
	{
		// Creating the new controller
		debugVerbose("creating new controller for this device\n");
		
		pController = new AOE_CONTROLLER_NAME;
		
		if ( NULL==pController )
		{
			debugError("pControllerToAdd is NULL\n");
			return -1;
		}
		
		pController->init(this, nMajor, nMinor, ifnet_receive, pEHeader->ether_shost, m_pAoEService->get_mtu(), m_nMaxTransferSize, get_next_target_number() );
		
		// Run the rest in the timeout, and exit now.
		
		if ( !pController->attach(this) )
		{
			debugError("Trouble attaching pController\n");
			CLEAN_RELEASE(pController);
			return -1;
		}
		
		if ( !pController->start(this) )
		{
			debugError("Trouble starting pController\n");
			pController->detach(this);
			CLEAN_RELEASE(pController);
			return -1;
		}
		
		m_pControllers->setObject(pController);
		
		// Update with info
		pController->update_target_info(ifnet_receive, pEHeader->ether_shost, TRUE);
		pController->handle_aoe_cmd(ifnet_receive, pCfgHeader, pMBufData);
		
		// Attach the device now, it wont be available until we register the disk
		
		pController->attach_device();
		// Check the config string is ours
		if ( 0==pController->cstring_is_ours(m_pAoEService->get_com_cstring()) )
		{
			debug("Config string belongs to us, registering service and mounting drive\n");
			
			pController->registerDiskService();
		}
		else
		{
			debug("Config string not recognised, not mounting drive\n");
		}
		
		// setObject will hold a reference to our controller, so we can release now.
		CLEAN_RELEASE(pController);
		
		// For new commands, we dont do any further handling of the AoE commands here
	}	
	
	return 0;
}




/*---------------------------------------------------------------------------
 * This timer is called to fake an ATA response. It's used for commands that are not supported by the
 * AoE specification, but are required for higher level drivers (ATA protocol)
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::FakeReturnTimer(OSObject* pOwner, IOTimerEventSource* pSender)
{
	AOE_CONTROLLER_INTERFACE_NAME* pAC = OSDynamicCast(AOE_CONTROLLER_INTERFACE_NAME, pOwner);
	
	debug("AOE_CONTROLLER_INTERFACE_NAME::FakeReturnTimer\n");
	if ( pAC )
	{
		aoe_atahdr_rd ATAHeader;
		
		AOE_ATAHEADER_CLEAR(&ATAHeader);
		ATAHeader.aa_scnt_cmdstat = 0x40<<8;			// Just requires the DRDY bit to be set high in the status register
		
		if ( pAC->m_pControllerToFakeResponse )
			pAC->m_pControllerToFakeResponse->ata_response(&ATAHeader, NULL, 0);
		else
			debugError("m_pControllerToFakeResponse not initialised\n");
	}
}




#pragma mark -
#pragma mark target online/offline

/*---------------------------------------------------------------------------
 * Check for any downed targets, removing the device from the system if it has gone down
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::check_down_targets()
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			if ( time_since_now_us(pController->time_since_last_comm()) >= m_TimeUntilTargetOffline_us )
			{
				debugVerbose("Target %d now OFFLINE. Hasn't been seen for %lums\n", pController->target_number(), time_since_now_ms(pController->time_since_last_comm()));
				remove_target(pController->target_number());
				
				// The iterator may be invalid now, we just break and catch any other down targets the next time...
				break;
			}
			else
			{
				debugVerbose("Target %d still ONLINE. Last spoke to target %lums ago...\n", pController->target_number(), time_since_now_ms(pController->time_since_last_comm()));
			}
		
		pControllerIterator->release();
	}	
}


/*---------------------------------------------------------------------------
 * Force all targets to send identify command
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::identify_all_targets()
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			pController->send_identify();

		pControllerIterator->release();
	}	
}


/*---------------------------------------------------------------------------
 * Count the number of targets currently available
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::number_of_targets(void)
{
	debug("number_of_targets = %d\n", m_pControllers->getCount());
	return m_pControllers->getCount();
}




/*---------------------------------------------------------------------------
 * Return info about a particular target
 ---------------------------------------------------------------------------*/
TargetInfo* AOE_CONTROLLER_INTERFACE_NAME::get_target_info(int nNumber)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
		{
			//debug("Checking target %d (looking for %d)\n", pController->target_number(), nNumber);
			if ( nNumber==pController->target_number() )
				return pController->get_target_info();
		}
		
		pControllerIterator->release();
	}
	
	return NULL;
}


/*---------------------------------------------------------------------------
 * Find the next available target number
 ---------------------------------------------------------------------------*/

int AOE_CONTROLLER_INTERFACE_NAME::get_next_target_number(void)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	int n;
	bool fFound;
	
	fFound = TRUE;
	n = 0;

	while ( fFound )
	{	
		fFound = FALSE;
		++n;
		
		pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
		if ( pControllerIterator )
		{
			while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			{
				if ( n==pController->target_number() )
				{	
					fFound = TRUE;
					break;
				}
			}
			
			pControllerIterator->release();
		}
	}

	return n;
}





/*---------------------------------------------------------------------------
 * Forcably remove a target from the system. NOTE: If the drive is still mounted, an OS warning will be triggered
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::remove_target(int nNumber)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	int nCount;
	bool fFound;

	nCount = 0;
	fFound = FALSE;

debug("AOE_CONTROLLER_INTERFACE_NAME::remove_target(%d)\n", nNumber);
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
		{
			if ( nNumber==pController->target_number() )
			{
				debug("Removing target: %d\n", nCount);
				
				// Cause any current commands to exit (and return an error)
				pController->cancel_command(FALSE);
				
				// Remove interfaces
				pController->remove_all_interfaces();
				
				// Begin teardown
				pController->uninit();
				pController->terminate();
				m_pControllers->removeObject(nCount);
				fFound = TRUE;
				break;
			}
			
			nCount++;
		}
		
		pControllerIterator->release();
	}
	
	if ( !fFound )
		debugError("Unable to find target in list\n");

	return fFound ? 0 : -1;
}




/*---------------------------------------------------------------------------
 * Cancel all outgoing commands on controllers that are connected to a particular interface
 *
 * NOTE: Regardless of the number of interfaces that may be active, if one of
 * them goes down, we cancel the whole command.
 *
 * The next command that is sent will send commands on a valid interface
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::cancel_commands_on_interface(ifnet_t enetifnet)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	debug("AOE_CONTROLLER_INTERFACE_NAME::cancel_commands_on_interface(%d)\n", enetifnet);
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
		{
			if ( 0==pController->connected_to_interface(enetifnet) )
			{
				debug("Cancelling command on target %d.%d\n", pController->get_target_info()->nShelf, pController->get_target_info()->nSlot);
				
				// Cause any current commands to exit (and return an error)
				pController->cancel_command(FALSE);
				break;
			}
		}
		
		pControllerIterator->release();
	}

	return 0;
}




/*---------------------------------------------------------------------------
 * Assign a particular config string to a target
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::set_targets_cstring(int nDevice, const char* pszConfigString, int nLength)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	int nRet = 0;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			if ( nDevice==pController->target_number() )
			{
				if ( 0==pController->set_config_string(pszConfigString, nLength) )
				{
					debug("Setting config string on device %d\n", nDevice);
					
					if ( 0 == pController->is_registered() )
					{
						debugVerbose("Device is already registered\n");
						
						// If the device is already registered and it doesn't belong to us, remove it
						if ( 0!=pController->cstring_is_ours(m_pAoEService->get_com_cstring()) )
						{
							debugWarn("Config string is not ours, removing target from the system\n");
							remove_target(pController->target_number());
							break;
						}
							
					}
					else
					{
						debugVerbose("Device is not registered\n");
						
						// Attach the device if it already isn't and the cstring is ours
						if ( 0==pController->cstring_is_ours(m_pAoEService->get_com_cstring()) )
						{
							debug("Config string belongs to us, registering service and mounting drive\n");
							
							pController->registerDiskService();
							break;
						}
					}
				}
				else
				{
					debug("Trouble sending packet to device, not setting config string\n");
					nRet = -1;
				}
			}
		
		pControllerIterator->release();
	}
	
	return nRet;
}


/*---------------------------------------------------------------------------
 * Determine if a targets interfaces are still active
 ---------------------------------------------------------------------------*/
bool AOE_CONTROLLER_INTERFACE_NAME::interfaces_active(TargetInfo* pTargetInfo)
{
	if ( m_pAoEService )
		return m_pAoEService->interfaces_active(pTargetInfo);
	return FALSE;
}


#pragma mark -
#pragma mark Forced Packet sends

/*---------------------------------------------------------------------------
 * Via the user interface, send a packet out the interface
 * The packet is tagged so as not to conflict with any other transactions that are in progress
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::force_packet_send(ForcePacketInfo* pForcedPacketInfo)
{
	bool fFoundDevice;
	int nShelf;
	int nSlot;
	
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	nShelf = pForcedPacketInfo->nShelf;
	nSlot = pForcedPacketInfo->nSlot;
	
	fFoundDevice = FALSE;
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		// Iterate through list, looking for our ID
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
		{
			if ( 0==pController->is_device(nShelf, nSlot) )
			{
				fFoundDevice = TRUE;
				
				// send command on to our device
				pController->force_packet_send(pForcedPacketInfo);
			}
		}
		
		pControllerIterator->release();
	}
	else
	{
		debugError("Unable to iterate through controller list\n");
	}
	
	// err if we didn't find the device in our list
	if ( !fFoundDevice )
		debugError("Device %d.%d not found, unable to send packet\n", nShelf, nSlot);
	
	return 0;
}





#pragma mark -
#pragma mark Outgoing packets

/*---------------------------------------------------------------------------
 * Send an mbuf packet through an interface
 * This routine adds the appropriate ethernet header to the packet based on the interfaces that
 * are available for a particular target
 * It also handles load-balancing by alternating the interfaces that each packet is sent out on
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::send_packet(mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo)
{
	int nInterfaceNumber;
	struct ether_header* eh;
	errno_t result;
	
	if ( NULL==pTargetInfo )
	{
		debugError("Invalid TargetInfo\n");
		return -1;
	}
	
	/*
	 * Prepend the ethernet header, we will send the raw frame;
	 * callee frees the original mbuf when allocation fails.
	 */
	result = mbuf_prepend(&m, sizeof(*eh), MBUF_WAITOK);
	if (result != 0)
		return result;
	
	eh = MTOD(m, struct ether_header*);
	eh->ether_type = htons(ETHERTYPE_AOE);
	
	if ( 0==pTargetInfo->nNumberOfInterfaces )
	{
		debugError("Device is offline. Dropping mbuf\n");
		mbuf_freem(m);
		m = NULL;
		return -1;
	}
	
	//~~~~~~~~~~~~~~~~//
	// Load balancing //
	//~~~~~~~~~~~~~~~~//
	
	// If multiple interfaces are available for a target, we alternate the interface we send on (provided they are enabled)
	do
	{
		nInterfaceNumber = (pTargetInfo->nLastSentInterface+1) % pTargetInfo->nNumberOfInterfaces;
		pTargetInfo->nLastSentInterface = nInterfaceNumber;
	}
	while ( !m_pAoEService->interface_active(pTargetInfo, nInterfaceNumber) );

	//debugVerbose("Sending on interface %d (%d enabled)\n", nInterfaceNumber, pTargetInfo->nNumberOfInterfaces);

	// Send to the mac address of the appropriate target (based on the interface we are sending out on)
	bcopy(pTargetInfo->aaDestMACAddress[nInterfaceNumber], eh->ether_dhost, sizeof(eh->ether_dhost));
	
	return m_pAoEService->send_packet_on_interface(pTargetInfo->aInterfaces[nInterfaceNumber], Tag, m, pTargetInfo->nShelf);	
}





/*---------------------------------------------------------------------------
 * Send an aoe packet out an interface
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::send_aoe_packet(AOE_CONTROLLER_NAME* pSender, mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo)
{
	debug("AOE_CONTROLLER_INTERFACE_NAME::send_aoe_packet\n");
	
	// No pre-processing is required, just pass it on to common function
	return send_packet(m, Tag, pTargetInfo);	
}





/*---------------------------------------------------------------------------
 * This function pre-processes outgoing commands so we can "fake" responses that aren't supported by the AoE targets
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_INTERFACE_NAME::send_ata_packet(AOE_CONTROLLER_NAME* pSender, mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo)
{
	aoe_atahdr_full* pAoEFullATAHeader;
	aoe_header*	pAoEHeader;
	aoe_atahdr* pATAhdr;
	
	pAoEFullATAHeader = MTOD(m, aoe_atahdr_full*);
	
	// Add shortcuts
	pAoEHeader = &(pAoEFullATAHeader->aoe);
	pATAhdr = &(pAoEFullATAHeader->ata);
	
	debug("Outgoing command is %#x (FEAT=%#x) [TAG=%#x]\n", AOE_ATAHEADER_GETSTAT(pATAhdr), AOE_ATAHEADER_GETERR(pATAhdr), Tag);
	
	// Some of the commands are handled without sending it to the target
	if (	(AOE_ATAHEADER_GETSTAT(pATAhdr)==kATAcmdSetFeatures) ||
		(AOE_ATAHEADER_GETSTAT(pATAhdr)==kATAcmdSleep) ||
		(AOE_ATAHEADER_GETSTAT(pATAhdr)==kATAcmdFlushCache) ||
		(AOE_ATAHEADER_GETSTAT(pATAhdr)==kATAcmdFlushCacheExtended) )
	{
		debug("Faking command response for outgoing command (%#x)\n", AOE_ATAHEADER_GETSTAT(pATAhdr));
		m_pControllerToFakeResponse = pSender;
		
		// Free mbuf as there's no need for it anymore
		mbuf_freem(m);
		
		// Run the reset in the timeout, and exit now.
		return m_pFakeReturnTimer->setTimeout(0.0);
	}
	
	return send_packet(m, Tag, pTargetInfo);
}





#pragma mark -
#pragma mark General state handling

/*---------------------------------------------------------------------------
 * Enable/Disable the search for targets
 ---------------------------------------------------------------------------*/

void AOE_CONTROLLER_INTERFACE_NAME::start_lun_search(bool fRun)
{
	if ( m_pStateUpdateTimer && (fRun!=m_fLUNSearchRunning) )
	{
		if ( fRun )
			m_pStateUpdateTimer->setTimeoutMS(LUN_UPDATE_TIME_MS);
		else
			m_pStateUpdateTimer->cancelTimeout();
	}

	m_fLUNSearchRunning = fRun;
}

/*---------------------------------------------------------------------------
 * Enable Controllers to accept commands.
 --------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::reenable_controllers(void)
{
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		// Iterate through list, enabling each controller
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			pController->device_online();
		
		pControllerIterator->release();
	}
}



/*---------------------------------------------------------------------------
 * Update the state of all targets
 ---------------------------------------------------------------------------*/

void AOE_CONTROLLER_INTERFACE_NAME::StateUpdateTimer(OSObject* pOwner, IOTimerEventSource* pSender)
{
	AOE_CONTROLLER_INTERFACE_NAME* pAC = OSDynamicCast(AOE_CONTROLLER_INTERFACE_NAME, pOwner);
	
	if ( pAC && pAC->m_pAoEService )
	{
		pAC->check_down_targets();
		pAC->m_pAoEService->aoe_search_all();
	}
	else
		debugError("Unable to search for targets. Config incorrect in AOE_CONTROLLER_INTERFACE_NAME\n");
	
	pSender->setTimeoutMS(LUN_UPDATE_TIME_MS);
}





#pragma mark -
#pragma mark Misc


/*---------------------------------------------------------------------------
 * Configure particular interface/shelf with a maximum outstanding count (determined by the "Buffer Count")
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding)
{
	// Just pass it up to the main service
	if ( m_pAoEService )
		m_pAoEService->set_max_outstanding(ifref, nShelf, nMaxOutstanding);
}


/*---------------------------------------------------------------------------
 * Set transfer size used for new interfaces
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::set_max_transfer_size(int nMaxTransferSize)
{
	debug("Setting max transfer size to %d\n", nMaxTransferSize);
	
	m_nMaxTransferSize = nMaxTransferSize;
}


/*---------------------------------------------------------------------------
 * Set MTU size used for existing Controllers
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_INTERFACE_NAME::adjust_mtu_sizes(int nMTU)
{
	debug("Setting all interfaces MTU size to %d\n", nMTU);
	
	OSCollectionIterator* pControllerIterator;
	AOE_CONTROLLER_NAME* pController;
	
	pControllerIterator = OSCollectionIterator::withCollection(m_pControllers);
    if ( pControllerIterator )
	{
		// Iterate through list, adjusting size of each controller
		while (pController = OSDynamicCast(AOE_CONTROLLER_NAME, pControllerIterator->getNextObject()))
			pController->set_mtu_size(nMTU);
		
		pControllerIterator->release();
	}
}



