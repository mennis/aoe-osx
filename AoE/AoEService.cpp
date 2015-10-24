/*
*  AoEService.cpp
*  AoE
*
* AoEService handles the communications on the various interfaces. There is only a single instance of this class
* although it may deal with an arbitrary number of ethernet interfaces.
*
* Various tasks of this class include:
* - Setting up interfaces for AoE
* - Sending packets on various interfaces
* - Receiving packets on interfaces
* - Slow Start/Congestion Control
* - Packet timeout handling, retransmission 
* 
* Recommended texts:
* - http://brantleycoilecompany.com/AoEr11.pdf
* - http://developer.apple.com/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Introduction/chapter_1_section_1.html
* - http://developer.apple.com/documentation/DeviceDrivers/Conceptual/MassStorage/01_Introduction/chapter_1_section_1.html
* - http://developer.apple.com/documentation/Darwin/Conceptual/NKEConceptual/intro/chapter_1_section_1.html
* - "Mac OS X Internals", A. Singh
* - "The design and implementation of the 4.4 BSD Operating System", M. McKusick et. al
* - "SATA Storage Technology", D. Anderson
* - "Ethernet, The definitive guide", C. Spurgeon
* - "Congestion Avoidance and Control", V. Jacobson
*
*  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
*
*/

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/ata/IOATATypes.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/OSAtomic.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include "AoEService.h"
#include "AoEtherFilter.h"
#include "../Shared/AoEcommon.h"
#include "AoEUserInterface.h"
#include "AoEControllerInterface.h"
#include "aoe.h"
#include "debug.h"

// BSD C functions
__BEGIN_DECLS
#include <net/kpi_interface.h>
#include <sys/kernel_types.h>
#include <sys/kpi_mbuf.h>
#include <sys/socket.h>
__END_DECLS

// Retransmit timer defaults
#define	RTO_MIN_NS								(1000*1000)
#define	RTO_MAX_NS								(10*1000*1000)
#define MAX_RETRANSMIT_TIMEOUT_US				(5*1000)
#define MAX_TIMEOUT_BEFORE_DROP_US				(60*1000*1000)

#define IDLE_DELAY_US							(5*1000*1000)

// Note: Since received commands are occuring when an ATA command is in progress, the command
//			gate will already open and thus it shouldn't be necessary to block when receiving
//			However, forcing this is required to ensure user commands don't interfere with
//			commands that are being received.
//			It may be possible to remove this, but more critical sections would have to be added to the code
#define USE_CG_FOR_INCOMING_PACKETS
#define TRIGGER_RETRANSMIT_WHEN_TX_COMPLETE

//#define NO_FLOW_CONTROL
#define DEBUG_RETRANSMIT
#define DEBUG_TRANSMIT
#define DEBUG_IDLE

#define super IOService
OSDefineMetaClassAndStructors(AOE_KEXT_NAME, IOService)


#pragma mark -
#pragma mark Standard IOService handling

/*---------------------------------------------------------------------------
 * The init, free, start, stop probe methods are the entry and exit points for this kext
 *
 * Since our provider class is IOResources, we're loaded immediately on startup.
 * For more info, see: http://developer.apple.com/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Matching/chapter_5_section_1.html
 ---------------------------------------------------------------------------*/

bool AOE_KEXT_NAME::init(OSDictionary *dict)
{
    bool res = super::init(dict);
    debugVerbose("Initializing\n");

	return res;
}




void AOE_KEXT_NAME::free(void)
{
    debugVerbose("Freeing kext\n");

    super::free();
}




IOService* AOE_KEXT_NAME::probe(IOService *provider, SInt32 *score)
{
    IOService* res;
    debugVerbose("Probing with score %d\n", *score);
	
	res = super::probe(provider, score);

    return res;
}





bool AOE_KEXT_NAME::start(IOService *provider)
{
	IOWorkLoop* pWorkLoop;
    bool res;
	
    debugVerbose("Starting\n");

	res = super::start(provider);
	if ( !res )
		goto Fail;

	res = !open_user_interface();

	if ( !res )
		goto Fail;

	m_pInterfaces = new EInterfaces(this);

	// Inform C functions of our location
	set_filtering_controller(this);			
	set_ui_controller(this);
	filter_init();

	m_pSentQueueMutex = IOLockAlloc();
	m_pToSendQueueMutex = IOLockAlloc();
	m_pGeneralMutex = IOLockAlloc();
	m_pszOurCString = (char*) IOMalloc(MAX_CONFIG_STRING_LENGTH);

	m_pAoEControllerInterface = new AOE_CONTROLLER_INTERFACE_NAME;
	
	if ( (m_pAoEControllerInterface==NULL) || !m_pAoEControllerInterface->init(this) )
	{
		debugError("Trouble initialising AoEController\n");
		return FALSE;
	}

	if ( !m_pAoEControllerInterface->attach(this) )
	{
		debugError("Trouble attaching AoEController\n");
		return kIOReturnError;
	}
	
	if ( !m_pAoEControllerInterface->start(this) )
	{
		debugError("Trouble starting AoEController\n");
		m_pAoEControllerInterface->detach(this);
		return FALSE;
	}
	
	m_pAoEControllerInterface->registerService();
	
	m_nRTO_ns = RTO_MAX_NS;
	m_MaxTimeOutBeforeDrop = MAX_TIMEOUT_BEFORE_DROP_US;
	m_nScaledRTTavg = 0;
	m_nScaledRTTvar = 0;
	m_nNumUnexpectedResponses = 0;
	m_nNumRetransmits = 0;
	
	TAILQ_INIT(&m_sent_queue);
	TAILQ_INIT(&m_to_send_queue);
	
	// Setup timers
	pWorkLoop = getWorkLoop();
	
	if ( pWorkLoop )
	{
		// Initialise Transmit timer
		m_pTransmitTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &AOE_KEXT_NAME::TransmitTimer);
		
		if ( !m_pTransmitTimer )
		{
			debugError("Unable to create timerEventSource\n");
			res = FALSE;
			goto Fail;
		}
		
		if ( pWorkLoop->addEventSource(m_pTransmitTimer) != kIOReturnSuccess )
		{
			debugError("Unable to add timerEventSource to work loop\n");
			res = FALSE;
			goto Fail;
		}
		m_pTransmitTimer->disable();

		// Initialise retransmit timer
		m_pRetransmitTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &AOE_KEXT_NAME::RetransmitTimer);
		
		if ( !m_pRetransmitTimer )
		{
			debugError("Unable to create timerEventSource\n");
			res = FALSE;
			goto Fail;
		}
		
		if ( pWorkLoop->addEventSource(m_pRetransmitTimer) != kIOReturnSuccess )
		{
			debugError("Unable to add timerEventSource to work loop\n");
			res = FALSE;
			goto Fail;
		}
		m_pRetransmitTimer->disable();

		// Initialise idle timer
		m_pIdleTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &AOE_KEXT_NAME::IdleTimer);
		
		if ( !m_pIdleTimer )
		{
			debugError("Unable to create timerEventSource\n");
			res = FALSE;
			goto Fail;
		}
		
		if ( pWorkLoop->addEventSource(m_pIdleTimer) != kIOReturnSuccess )
		{
			debugError("Unable to add timerEventSource to work loop\n");
			res = FALSE;
			goto Fail;
		}
		m_pIdleTimer->disable();		
		
		// Initialise the command gate
		m_pCmdGate = IOCommandGate::commandGate(this);
		
		if ( !m_pCmdGate || pWorkLoop->addEventSource(m_pCmdGate))
		{
			debugError("IOCommandGate failed\n");
			return false;
		}
	}
	else
		debugError("Unable to find work loop\n");
		
    res = TRUE;
	
Fail:
	if ( !res )
		close_user_interface();

	registerService();
	return res;
}




void AOE_KEXT_NAME::stop(IOService *provider)
{
	IOWorkLoop* pWorkLoop;
	struct SentPktQueue*	pSent_queue_item;
	struct ToSendPktQueue*	pToSend_queue_item;

    debugVerbose("Stopping...\n");

	// Stop timers and remove the command gate prior to shutting the rest of the system down (otherwise we could deadlock)
	pWorkLoop = getWorkLoop();
	
	if ( pWorkLoop )
	{
		// Remove timers
		if ( m_pTransmitTimer )
		{
			m_pTransmitTimer->cancelTimeout();
			pWorkLoop->removeEventSource(m_pTransmitTimer);
			CLEAN_RELEASE(m_pTransmitTimer);
		}
		
		if ( m_pRetransmitTimer )
		{
			m_pRetransmitTimer->cancelTimeout();
			pWorkLoop->removeEventSource(m_pRetransmitTimer);
			CLEAN_RELEASE(m_pRetransmitTimer);
		}
		
		if ( m_pIdleTimer )
		{
			m_pIdleTimer->cancelTimeout();
			pWorkLoop->removeEventSource(m_pIdleTimer);
			CLEAN_RELEASE(m_pIdleTimer);
		}
		
		// Remove Command gate from our work loop
		if ( m_pCmdGate )
		{
			pWorkLoop->removeEventSource(m_pCmdGate);
			CLEAN_RELEASE(m_pCmdGate);
		}
	}
	
	close_user_interface();
	filter_uninit();
	
	// We're done with these references now
	set_filtering_controller(NULL);
	set_ui_controller(NULL);
	
	if ( m_pAoEControllerInterface )
	{
		m_pAoEControllerInterface->uninit();
		m_pAoEControllerInterface->terminate();
		CLEAN_RELEASE(m_pAoEControllerInterface);
	}
	
	// Empty any data we may have in our sent queue
	debugVerbose("Empty sent queue...\n");
	IOLockLock(m_pSentQueueMutex);
	while ( !TAILQ_EMPTY(&m_sent_queue) )
	{
		// get the next item on the input side queue
		pSent_queue_item = TAILQ_FIRST(&m_sent_queue);
		remove_from_queue(pSent_queue_item);
	}
	IOLockUnlock(m_pSentQueueMutex);

	debugVerbose("Empty send queue...\n");
	IOLockLock(m_pToSendQueueMutex);
	while ( !TAILQ_EMPTY(&m_to_send_queue) )
	{
		// get the next item on the input side queue
		pToSend_queue_item = TAILQ_FIRST(&m_to_send_queue);
		// Free the mbuf as it's too late to send now
		mbuf_freem(pToSend_queue_item->mbuf);
		remove_from_queue(pToSend_queue_item);
	}
	IOLockUnlock(m_pToSendQueueMutex);
	
	IOLockFree(m_pSentQueueMutex);
	m_pSentQueueMutex = NULL;
	IOLockFree(m_pToSendQueueMutex);
	m_pToSendQueueMutex = NULL;
	IOLockFree(m_pGeneralMutex);
	m_pGeneralMutex = NULL;

	IOFree(m_pszOurCString, MAX_CONFIG_STRING_LENGTH);
	
	removeProperty(ENABLED_INTERFACES_PROPERTY);
	removeProperty(OUR_CSTRING_PROPERTY);
	
	delete m_pInterfaces;
	m_pInterfaces = NULL;

	debugVerbose("all done...\n");
    super::stop(provider);
}





#pragma mark -
#pragma mark Enabling/Disabling interfaces



/*---------------------------------------------------------------------------
 * Enable the interface and start searching for targets
 ---------------------------------------------------------------------------*/
kern_return_t AOE_KEXT_NAME::enable_interface(int nEthernetNumber)
{
	debug("enable_interface(%d) waiting for command gate\n", nEthernetNumber);

	return m_pCmdGate->runAction( (IOCommandGate::Action) &AOE_KEXT_NAME::cg_enable_interface, (void*) &nEthernetNumber, NULL, NULL, NULL);
}


void AOE_KEXT_NAME::cg_enable_interface(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/)
{
	AOE_KEXT_NAME* pOwner;
	int* pnEthernetNumber;

	debug("cg_enable_interface\n");
	
	pOwner = (AOE_KEXT_NAME*) owner;
	pnEthernetNumber = (int*) arg0;

	if ( (NULL==pOwner) || (NULL==pOwner->m_pInterfaces) || (NULL==pOwner->m_pAoEControllerInterface) )
	{
		debugError("Failure trying to enable interface\n");
		return;
	}	

	if ( KERN_SUCCESS==pOwner->m_pInterfaces->enable_interface(*pnEthernetNumber) )
	{
		// Adjust MTU size of all the interfaces already connected
		pOwner->m_pAoEControllerInterface->adjust_mtu_sizes(pOwner->get_mtu());

		pOwner->m_pAoEControllerInterface->reenable_controllers();

		// Start the periodic polling for new targets
		pOwner->m_pAoEControllerInterface->start_lun_search(TRUE);
	}
	else
		debugError("Failed to enabled interface\n");
}





/*---------------------------------------------------------------------------
 * Disable the interface and remove any outstanding commands
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::interface_disconnected(int nEthernetNumber)
{
	debug("interface_disconnected(%d) waiting for command gate\n", nEthernetNumber);

	m_pCmdGate->runAction( (IOCommandGate::Action) &AOE_KEXT_NAME::cg_disable_interface, (void*) &nEthernetNumber, NULL, NULL, NULL);
}

void AOE_KEXT_NAME::cg_disable_interface(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/)
{
	AOE_KEXT_NAME* pOwner;
	int* pnEthernetNumber;
	struct ToSendPktQueue*	pToSend_queue_tmp;
	struct ToSendPktQueue*	pToSend_queue_item;
	struct SentPktQueue*	pSent_queue_item;
	struct SentPktQueue*	pSent_queue_tmp;
	ifnet_t Interface;

	debug("cg_disable_interface\n");

	pOwner = (AOE_KEXT_NAME*) owner;
	pnEthernetNumber = (int*) arg0;
	
	if ( (NULL==pOwner) || (NULL==pOwner->m_pRetransmitTimer) || (NULL==pOwner->m_pInterfaces) || (NULL==pOwner->m_pAoEControllerInterface) )
	{
		debugError("Failure trying to disable interface\n");
		return;
	}

	// Disable our timers temporarily
	pOwner->m_pRetransmitTimer->disable();
	pOwner->m_pTransmitTimer->disable();
	
	debug("interface_disconnected\n");
	Interface = pOwner->m_pInterfaces->get_nth_interface(*pnEthernetNumber);
	pOwner->m_pInterfaces->interface_disconnected(*pnEthernetNumber);

	debugVerbose("Purging send queue for this interface\n");
	IOLockLock(pOwner->m_pToSendQueueMutex);
	TAILQ_FOREACH_SAFE(pToSend_queue_item, &pOwner->m_to_send_queue, q_next, pToSend_queue_tmp)
	{
		if ( pToSend_queue_item && (1!=pOwner->m_pInterfaces->is_used(pToSend_queue_item->if_sent)) )
		{
			debug("\tremoving from queue...\n");
			pOwner->remove_from_queue(pToSend_queue_item);
		}
	}
	IOLockUnlock(pOwner->m_pToSendQueueMutex);

	debugVerbose("Purging sent queue for this interface\n");
	IOLockLock(pOwner->m_pSentQueueMutex);
	TAILQ_FOREACH_SAFE(pSent_queue_item, &pOwner->m_sent_queue, q_next, pSent_queue_tmp)
	{
		if ( pSent_queue_item && (1!=pOwner->m_pInterfaces->is_used(pSent_queue_item->if_sent)) )
		{
			debugVerbose("\tremoving from queue...\n");
			pOwner->remove_from_queue(pSent_queue_item);
		}
	}
	IOLockUnlock(pOwner->m_pSentQueueMutex);

	if ( pSent_queue_item )
		*pSent_queue_item->pOutstandingCount = 0;

	// Re-enable timers now that we've cleared our queues for the interface
	// If there is nothing to do, they'll just exit anyway, but we need to make
	// sure we dont stop traffic on a different interface
	pOwner->enable_transmit_timer();
	pOwner->enable_retransmit_timer();

	// Cancel any commands that are currently in progress
	pOwner->m_pAoEControllerInterface->cancel_commands_on_interface(Interface);
}


UInt32 AOE_KEXT_NAME::get_mtu(void)
{
	return m_pInterfaces->get_mtu();
}


UInt32 AOE_KEXT_NAME::get_sector_count(void)
{
	return m_pInterfaces->get_mtu() ? COUNT_SECTORS_FROM_MTU(m_pInterfaces->get_mtu()) : 0;
}


int AOE_KEXT_NAME::get_payload_size(UInt32* pPayloadSize)
{
	if ( pPayloadSize )
		*pPayloadSize = kATADefaultSectorSize*get_sector_count();

	return 0;
}


kern_return_t AOE_KEXT_NAME::disable_interface(int nEthernetNumber)
{
	debug("interface en%d disabled\n", nEthernetNumber);
	
	interface_disconnected(nEthernetNumber);
	
	// Disable packet filtering
	return disable_filtering(nEthernetNumber);
}


void AOE_KEXT_NAME::interface_reconnected(int nEthernetNumber, ifnet_t enetifnet)
{
	debug("interface en%d reconnected\n", nEthernetNumber);
	
	if ( m_pInterfaces )
		m_pInterfaces->interface_reconnected(nEthernetNumber, enetifnet);

	if ( m_pAoEControllerInterface )
	{
		m_pAoEControllerInterface->adjust_mtu_sizes(get_mtu());
		m_pAoEControllerInterface->reenable_controllers();
	}
}




/*---------------------------------------------------------------------------
 * Assign our computers config string to the kext (stored in the registry)
 * This is later used to mount drives that have the same config string as us
 ---------------------------------------------------------------------------*/
int AOE_KEXT_NAME::set_our_cstring(const char* pszOurCString)
{
	OSString* pOurCString;
	
	removeProperty(OUR_CSTRING_PROPERTY);

	pOurCString = OSString::withCString(pszOurCString);
	if ( pOurCString )
	{
		setProperty(OUR_CSTRING_PROPERTY, (OSObject* )pOurCString);
		pOurCString->release();
	}

	memcpy(m_pszOurCString, pszOurCString, MAX_CONFIG_STRING_LENGTH);
	
	return (pOurCString!=NULL) ? 0 : -1;
}


int AOE_KEXT_NAME::set_max_transfer_size(int nMaxSize)
{
	if ( m_pAoEControllerInterface )
		m_pAoEControllerInterface->set_max_transfer_size(nMaxSize);
	
	return (m_pAoEControllerInterface!=NULL) ? 0 : -1;
}


int AOE_KEXT_NAME::set_user_window(int nMaxSize)
{
	if ( m_pInterfaces )
		m_pInterfaces->m_nMaxUserWindow = nMaxSize;

	return 0;
}





#pragma mark -
#pragma mark handle incoming/outgoing packets

/*---------------------------------------------------------------------------
 * This is the main entry for an incoming ethernet packet. There are two options here,
 * process on the command gate, or process in parallel. The command gate is safest, but 
 * avoiding it might be slightly more efficient. I've left this as a define to play around
 * with the two methods, although not using the command gate needs some work as occasionally
 * the kext will panic.
 *
 * For more on command gates and event sources, see:
 * http://developer.apple.com/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/HandlingEvents/chapter_8_section_3.html
 *
 * Calling this with through the command gate ensures single threaded access in the driver.
 * We could make this even more multi-threaded by considering a separate work loop for each ATAController.
 * 
 ---------------------------------------------------------------------------*/
int AOE_KEXT_NAME::aoe_incoming(ifnet_t ifp, struct ether_header* pEHeader, mbuf_t* pMBufData)
{
#ifndef USE_CG_FOR_INCOMING_PACKETS
	cg_aoe_incoming((OSObject*)this, (void*) ifp, (void*) pEHeader, (void*) pMBufData, 0);
#else
	// Here, the actual processing is handled on the command gate
	m_pCmdGate->runAction( (IOCommandGate::Action) 
							&AOE_KEXT_NAME::cg_aoe_incoming,
							(void*) ifp,				// arg 0
							(void*) pEHeader,			// arg 1
							(void*) pMBufData,			// arg 2
							0);							// arg 3
#endif

	return 0;
}





/*---------------------------------------------------------------------------
 * Static function called by the internal IOCommandGate object to handle a runAction() request invoked by aoe_incoming().
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::cg_aoe_incoming(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/)
{
	AOE_KEXT_NAME* pThis;
	ifnet_t ifp;
	struct ether_header* pEHeader;
	mbuf_t* pMBufData;
	struct SentPktQueue* pTlq;
	struct SentPktQueue* pTlqNext;
	aoe_header* pAoEFullHeader;
	aoe_cfghdr_rd* pCfgHeader;
	aoe_atahdr_rd* pATAHeader;
	UInt32		IncomingPacketTag;
	bool		fPacketFound;
	int			nPrevCWND;
	
	debug("cg_aoe_incoming-ININININININININ\n");

	pThis = OSDynamicCast(AOE_KEXT_NAME, owner);
	ifp = (ifnet_t) arg0;
	pEHeader = (struct ether_header*) arg1;
	pMBufData = (mbuf_t*) arg2;

	if ( NULL==pThis )
		return;

	//-----------------------------//
	// Check our sent packet queue //
	//-----------------------------//
	if ( 0==pMBufData )
	{
		debugError("pMBufData is invalid\n");
		return;
	}

	pAoEFullHeader = MTOD(*pMBufData, aoe_header*);
	
	if ( 0==pAoEFullHeader )
	{
		debugError("pMBufData data is invalid\n");
		return;
	}

	IncomingPacketTag = AOE_HEADER_GETTAG(pAoEFullHeader);

	//debugVerbose("&&&&Looking for packet with tag: %#x\n", IncomingPacketTag);
	
	fPacketFound = FALSE;
	if ( AOE_HEADER_GETFLAG(pAoEFullHeader)&AOE_FLAG_RESPONSE )
	{
		IOLockLock(pThis->m_pSentQueueMutex);
		//debugVerbose("&&&&Iterating over our queue for tag: %#x\n", IncomingPacketTag);
		for (pTlq = TAILQ_FIRST(&pThis->m_sent_queue); pTlq != NULL; pTlq = pTlqNext)
		{
			// get the next element pointer before we potentially corrupt it
			pTlqNext = TAILQ_NEXT(pTlq, q_next);

			//if ( pTlq )
			//	debugVerbose("&&&&Queued packet has tag: %#x\n", pTlq->Tag);

			// Check that the incoming packet is the same as this one
			if ( pTlq && (pTlq->Tag==IncomingPacketTag) )
			{
				//debugVerbose("&&&&Found the packet with tag: %#x\n", pTlq->Tag);

				// Update the number of outstanding commands
				if ( pTlq->pOutstandingCount )
				{
					if ( !pTlq->fPacketHasBeenRetransmit )
						OSDecrementAtomic(pTlq->pOutstandingCount);
					else
						debug("Not decrementing outstanding count as this packet was retransmit\n");

					debugVerbose("RCV-Outstanding replies on this interface=%d (TAG=%#x)\n", *pTlq->pOutstandingCount, pTlq->Tag);

					// Quick check for validity
					if ( *pTlq->pOutstandingCount<0 )
					{
						debugError("Invalid Outstanding count. Resetting to zero\n");
						*pTlq->pOutstandingCount = 0;
					}
				}

				// Calculate the round trip time (rtt)
				if ( !pTlq->fPacketHasBeenRetransmit )
					pThis->update_rto(time_since_now_ns(pTlq->TimeSent));
				
				// We're done with this packet now
				pThis->remove_from_queue(pTlq);
				
				//---------------------------------//
				// Slow Start / Congestion control //
				//---------------------------------//

				nPrevCWND = pThis->m_pInterfaces->get_cwnd(pTlq->if_sent);
				
				if ( nPrevCWND < pThis->m_pInterfaces->get_ssthresh(pTlq->if_sent) )
					pThis->m_pInterfaces->grow_cwnd(pTlq->if_sent, 1, 0);	// Exponential growth  (cwnd+=1)
				else
					pThis->m_pInterfaces->grow_cwnd(pTlq->if_sent, 0, 1);	// Fractional growth (cwnd+=1/cwnd)
				
				fPacketFound = TRUE;
				break;
			}
			//else
				//if ( pTlq )
				//	debugVerbose("&&&&Nope, that ain't it. On to the next one.\n");
		}
		IOLockUnlock(pThis->m_pSentQueueMutex);
	}
	else
	{
		// This is a normal case, but it's not our packet, so just exit
		return;
	}
	
//	if ( !fPacketFound )
//		debugVerbose("&&&Hmm, couldn't find our packet in the queue.\n");
	
	//--------------------------//
	// Handle unexpected packet //
	//--------------------------//
	
	if ( AOE_HEADER_GETFLAG(pAoEFullHeader)&AOE_FLAG_ERROR )
	{
		switch ( AOE_HEADER_GETERR(pAoEFullHeader) )
		{
			case 1:
				debugError("AoE protocol error on packet %#x (Error %d - Unrecognised command code)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
			case 2:
				debugError("AoE protocol error on packet %#x (Error %d - Bad argument parameter)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
			case 3:
				debugError("AoE protocol error on packet %#x (Error %d - Device Unavailable)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
			case 4:
				debugError("AoE protocol error on packet %#x (Error %d - Config String Present)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
			case 5:
				debugError("AoE protocol error on packet %#x (Error %d - Unsupported version)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
			default:
				debugError("AoE protocol error on packet %#x (Error %d - Unknown error)\n", IncomingPacketTag, AOE_HEADER_GETERR(pAoEFullHeader));
				break;
		}
		++pThis->m_nNumUnexpectedResponses;
		return;
	}

	//----------------------------//
	// Parse our incoming packet: //
	//----------------------------//

	if ( fPacketFound || (DEVICE_ONLINE_TAG==IncomingPacketTag) || (TAG_BROADCAST_MASK&IncomingPacketTag) )
	{
		debug("AoE Command received!!!\n");

		if ( DEVICE_ONLINE_TAG==IncomingPacketTag )
			debug("Targets have just come online\n");
			
		debug("Ver=%d Flags=%x error=%#x major=%#x minor=%#x command=%#x, Tag=%#x\n",
				   AOE_HEADER_GETVER(pAoEFullHeader),
				   AOE_HEADER_GETFLAG(pAoEFullHeader),
				   AOE_HEADER_GETERR(pAoEFullHeader),
				   AOE_HEADER_GETMAJOR(pAoEFullHeader),
				   AOE_HEADER_GETMINOR(pAoEFullHeader),
				   AOE_HEADER_GETCMD(pAoEFullHeader),
				   AOE_HEADER_GETTAG(pAoEFullHeader));

		if ( AOE_SUPPORTED_VER!=AOE_HEADER_GETVER(pAoEFullHeader) )
			debugError("Unexpected Version\n");
		
		switch ( AOE_HEADER_GETCMD(pAoEFullHeader) )
		{
			case AOE_ATA_COMMAND :
			{
				pATAHeader = &((aoe_atahdr_rd_full*)pAoEFullHeader)->ata;
				
				if ( pThis->m_pAoEControllerInterface )
					pThis->m_pAoEControllerInterface->aoe_ata_receive(pAoEFullHeader, pATAHeader, pMBufData);	
				break;
			}
			case AOE_CFG_COMMAND :
			{
				// The AoEController will handle the command parsing here
				pCfgHeader = &((aoe_cfghdr_rd_full*)pAoEFullHeader)->cfg;
				if ( pThis->m_pAoEControllerInterface )
					pThis->m_pAoEControllerInterface->aoe_config_receive(ifp, pEHeader, pAoEFullHeader, pCfgHeader, pMBufData);
				break;
			}
			default :
				// Silently ignore AoE commands and command responses that are unrecognised vendor extensions
				++pThis->m_nNumUnexpectedResponses;
				break;
		}

		//-----------------------//
		// Check our MBUF flags: //
		//-----------------------//

		// Presently doesn't support multiple packets, but we shouldn't receive those anyway
		if ( NULL != mbuf_nextpkt(*pMBufData) )
			debugError("Note, incoming data has additional MBUF packets. Throwing away data...\n");
	}
	else
	{
		debugVerbose("Dropping incoming packet with tag %#x as it's not found in our sent queue.\n", IncomingPacketTag);
		++pThis->m_nNumUnexpectedResponses;
	}

	// Since we have more room in our window, send more data if we have more to send
	if ( fPacketFound && !TAILQ_EMPTY(&pThis->m_to_send_queue) )
		pThis->enable_transmit_timer();

	debug("cg_aoe_incoming-OUTOUTOUTOUTOUTOUTOUTOUT\n");
	
	return;
}




/*---------------------------------------------------------------------------
 * "Forced packets" have come from the AoE user interface. This is only used for testing. We send the commands
 * through the command gate to ensure exclusive access (no other sends/receives are in progress)
 ---------------------------------------------------------------------------*/
int AOE_KEXT_NAME::force_packet(ForcePacketInfo* pForcedPacketInfo)
{
	return m_pCmdGate->runAction( (IOCommandGate::Action) &AOE_KEXT_NAME::cg_force_packet, (void*) pForcedPacketInfo, NULL, NULL, NULL);
}





/*---------------------------------------------------------------------------
 * Static function called by the internal IOCommandGate object to handle a runAction() request invoked by force_packet().
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::cg_force_packet(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/)
{
	ForcePacketInfo* pForcedPacketInfo = (ForcePacketInfo*) arg0;
	AOE_KEXT_NAME* pOwner = (AOE_KEXT_NAME*) owner;
	
	if ( pOwner && pOwner->m_pAoEControllerInterface )
		pOwner->m_pAoEControllerInterface->force_packet_send(pForcedPacketInfo);
	else
		debugError("Trouble sending forced packet\n");
}





#pragma mark -
#pragma mark Target handling

/*---------------------------------------------------------------------------
 * Return info to user interface about the targets that are currently connected
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::find_targets(int* pnTargets)
{
	errno_t ret;
	
	// Force to zero if our search fails (which will happen if we haven't enabled any interfaces)
	if ( pnTargets )
		*pnTargets = 0;
	
	// Ensure we're up-to-date
	ret = aoe_search_all();

	if ( ret != 0 )
		return ret;
	
	// Pause while we wait for the commands to come in (not 100% robust, but the targets are updated periodically anyway)
	IOSleep(RTO_MAX_NS/1000000);
	
	if ( pnTargets && m_pAoEControllerInterface )
		*pnTargets = m_pAoEControllerInterface->number_of_targets();
	return 0;
}




/*---------------------------------------------------------------------------
 * Return info to user interface about the targets that are currently connected
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::get_target_info(int nDevice, TargetInfo* pTargetData)
{
	TargetInfo* pData;
	
	pData = NULL;

	debugVerbose("get_target_info (device=%d)...\n", nDevice);
	
	if ( pTargetData && m_pAoEControllerInterface )
	{
		pData = m_pAoEControllerInterface->get_target_info(nDevice);
		
		// Copy structure's data to our own
		if ( pData )
		{
			*pTargetData = *pData;
		}
		else
		{
			debugError("Unable to find target info for device #%d\n", nDevice);
			pTargetData = NULL;
			return -1;
		}
	}
	
	return 0;
}


/*---------------------------------------------------------------------------
 * Assign a c-string to a particular target (called from user space)
 * This is sent through the command gate
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::set_targets_cstring(ConfigString* pCStringInfo)
{
	return m_pCmdGate->runAction( (IOCommandGate::Action) &AOE_KEXT_NAME::cg_set_targets_cstring, (void*) pCStringInfo, NULL, NULL, NULL);
}


/*---------------------------------------------------------------------------
 * Static function called by the internal IOCommandGate object to handle a runAction() request invoked by set_targets_cstring().
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::cg_set_targets_cstring(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/)
{
	ConfigString* pCStringInfo = (ConfigString*) arg0;
	AOE_KEXT_NAME* pOwner = (AOE_KEXT_NAME*) owner;
	
	if ( pOwner && pOwner->m_pAoEControllerInterface )
		pOwner->m_pAoEControllerInterface->set_targets_cstring(pCStringInfo->nTargetNumber, (const char*) pCStringInfo->pszConfig, pCStringInfo->Length);
}



/*---------------------------------------------------------------------------
 * Seach for active targets on all our interfaces
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::aoe_search_all(void)
{
	errno_t	result = 0;
	ifnet_t	ifn;
	int n;
	
	// Iterate over all enabled interfaces searching for active targets
	if ( m_pAoEControllerInterface )
	{
		for(n=0; ifn = m_pInterfaces->get_nth_interface(n); n++ )
		{
			result = m_pAoEControllerInterface->aoe_search(ifn);
			if ( result != 0 )
				return result;
		}

		m_pAoEControllerInterface->identify_all_targets();
	}

	return result;
}


/*---------------------------------------------------------------------------
 * Determine if a targets interfaces are still active
 ---------------------------------------------------------------------------*/
bool AOE_KEXT_NAME::interfaces_active(TargetInfo* pTargetInfo)
{
	bool fInterfacesActive;
	int n;

	fInterfacesActive = FALSE;

	if ( NULL==pTargetInfo )
		return FALSE;

	// Check if any of the interfaces are in use. If one of them is, we're good to go...
	for(n=0; n<pTargetInfo->nNumberOfInterfaces; n++)
		if ( TRUE==m_pInterfaces->is_used(pTargetInfo->aInterfaces[n]) )
			fInterfacesActive = TRUE;

	return fInterfacesActive;
}

/*---------------------------------------------------------------------------
 * Determine if a particular targets interface is still active
 ---------------------------------------------------------------------------*/
bool AOE_KEXT_NAME::interface_active(TargetInfo* pTargetInfo, int nInterfaceNumber)
{
	if ( NULL==pTargetInfo )
		return FALSE;

	return (TRUE==m_pInterfaces->is_used(pTargetInfo->aInterfaces[nInterfaceNumber]));
}

#pragma mark -
#pragma mark Flow Control


/*---------------------------------------------------------------------------
 * Set/get values for a particular interface.
 * The max outstanding can also be set for a particular interface/shelf (ie. buffer count)
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding)
{
	m_pInterfaces->set_max_outstanding(ifref, nShelf, nMaxOutstanding);
}

int AOE_KEXT_NAME::get_outstanding(ifnet_t ifref)
{
	return m_pInterfaces->get_outstanding(ifref);
}




/*---------------------------------------------------------------------------
 * Given a round trip time, we calculate the value of the retransmit timer.
 * See: V.Jacobson, M.Karels: "Congestion Avoidance and Control", Nov 1988
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::update_rto(uint64_t nRTT)
{
	const int nRTTin = nRTT;
	
	IOLockLock(m_pGeneralMutex);
	
	// The code is similar to appendix-A of the above article (but without the bugs!):
	int nErr = (nRTT-m_nScaledRTTavg);
	m_nScaledRTTavg += nErr>>3;
	if ( nErr < 0 )
		nErr = -nErr;
	nErr -= m_nScaledRTTvar;
	m_nScaledRTTvar += nErr>>2;
	
	m_nRTO_ns = m_nScaledRTTavg + (m_nScaledRTTvar<<2);
	debug("UPDATE ROUND TRIP TIME - nRTT=%luus | Avg=%luus | Var=%luus [RTO=%luus]\n", CONVERT_NS_TO_US(nRTTin), CONVERT_NS_TO_US(m_nScaledRTTavg), CONVERT_NS_TO_US(m_nScaledRTTvar), CONVERT_NS_TO_US(m_nRTO_ns));
	
	IOLockUnlock(m_pGeneralMutex);
}




/*---------------------------------------------------------------------------
 * Return the retransmit timer (in us) based on the estimation of round trip times
 ---------------------------------------------------------------------------*/

UInt64 AOE_KEXT_NAME::get_rto_us(void)
{
	return CONVERT_NS_TO_US(MAX(m_nRTO_ns, RTO_MIN_NS));
}

UInt64 AOE_KEXT_NAME::get_max_timeout_before_drop(void)
{
	return m_MaxTimeOutBeforeDrop;
}




/*---------------------------------------------------------------------------
 * Resend a packet. This takes an old item from the sent queue and replaces it on the send queue
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::resend_packet(struct SentPktQueue* pSent_queue_item)
{
	mbuf_t	mbuf_to_send;
	
	if ( pSent_queue_item )
	{
		// Increase the next timeout
		pSent_queue_item->RetransmitTime_us = MIN(2*pSent_queue_item->RetransmitTime_us, MAX_RETRANSMIT_TIMEOUT_US);

		debug("\t\tRESEND PACKET with TAG=%#x and updating timeout to %luus\n", pSent_queue_item->Tag, pSent_queue_item->RetransmitTime_us);

		// Update Time (again, we set this to zero so it gets updated when it's actually sent from the send queue)
		pSent_queue_item->TimeSent = 0;
		
		// This packet is an anomoly, exclude the packet from RTT calculations
		pSent_queue_item->fPacketHasBeenRetransmit = TRUE;

		// Make another copy of the mbuf before we lose it in the output routine
		mbuf_dup(pSent_queue_item->first_mbuf, MBUF_WAITOK, &mbuf_to_send);
		
		// resend immediately
		add_to_send_queue(pSent_queue_item->if_sent, pSent_queue_item->Tag, mbuf_to_send, pSent_queue_item->nShelf, pSent_queue_item->pOutstandingCount, TRUE /*immediate send*/);
		
		// Increment the retransmit count if we have previously found at least one target
		if ( m_pAoEControllerInterface && (m_pAoEControllerInterface->number_of_targets() > 0) )
			++m_nNumRetransmits;
	}
}






#pragma mark -
#pragma mark Timer handling

/*---------------------------------------------------------------------------
 * Starts the retransmit timer. This is called after every transmit.
 * There are two ways of handling this:
 * TRIGGER_RETRANSMIT_WHEN_TX_COMPLETE	- Restart the timer every time we're called
 * not above							- Leave time in place
 *
 * The latter method will call retransmit more often, and sticks closer to the RTO values when retransmitting
 * The first method (the default) isn't as aggressive and holds off retransmits slightly while there are a lot
 * of transmits occuring. This often avoids unnecessary retransmits and thus is slightly faster
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::enable_retransmit_timer(void)
{
	UInt64 lDelay;

	lDelay = get_rto_us();

#ifdef DEBUG_RETRANSMIT
	debug("Setting retransmit timer with %luus delay\n", lDelay);
#endif	
	
#ifdef TRIGGER_RETRANSMIT_WHEN_TX_COMPLETE
	// Restart out retransmit timer
	if ( m_pRetransmitTimer->isEnabled() )
		m_pRetransmitTimer->disable();

	m_pRetransmitTimer->enable();
	m_pRetransmitTimer->setTimeoutUS(lDelay);
#else
	if ( !m_pRetransmitTimer->isEnabled() )
	{
		m_pRetransmitTimer->enable();
		m_pRetransmitTimer->setTimeoutUS(lDelay);
	}
#endif
}





/*---------------------------------------------------------------------------
 * Enabled transmit timer. If the timer isn't already in progress, the timer is started.
 * Optionally, the caller can delay the transmit by passing a delay.
 * If the timer is already running, this value is ignored.
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::enable_transmit_timer(int nDelaySend_us /*=?*/)
{
	// If it's not already in progress, start our retransmit timer
	if ( !m_pTransmitTimer->isEnabled() )
	{
		m_pTransmitTimer->enable();
		m_pTransmitTimer->setTimeoutUS(nDelaySend_us);

		#ifdef DEBUG_TRANSMIT
		debug("Setting transmit timer with %luus delay\n", nDelaySend_us);
		#endif
	}
	else
		debug("Not re-arming transmit timer as it's already enabled....\n");
}






/*---------------------------------------------------------------------------
 * Actually handle any transmits.
 * If the slow-start/congestion control allows it, this iterates over our send queue
 * sending packets. To avoid holding up our event source, we only transmit a single packet
 * in this function. If there are packets remaining after sending, it re-enables the transmit
 * timer at the end of the function. This gives the receive routine a chance to actually receive
 * packets 
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::TransmitTimer(OSObject* pOwner, IOTimerEventSource* pSender)
{
	AOE_KEXT_NAME*			pThis;
	struct ToSendPktQueue*	pToSend_queue_tmp;
	struct ToSendPktQueue*	pToSend_queue_item;
	int						nOutstanding;
	int						nCWND;
	int						nMaxForThisShelf;
	bool					fMoreToSend;
	int						nMaxoutstanding;
	UInt64					IdleTime_us;
	
	pThis = OSDynamicCast(AOE_KEXT_NAME, pOwner);
	
#ifdef DEBUG_TRANSMIT
	debugVerbose("TransmitTimer FIRED%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%!!\n");
#endif
	
	// Allow the re-transmit timer to be called again. This is done at the beginning because resend_packet will re-arm the timer
	pSender->disable();

	fMoreToSend = FALSE;

	if ( pThis )
	{
		IOLockLock(pThis->m_pToSendQueueMutex);

		// Quickly iterate over our queue to see if any packets need to be sent immediately
		TAILQ_FOREACH_SAFE(pToSend_queue_item, &pThis->m_to_send_queue, q_next, pToSend_queue_tmp)
		{
			if ( pToSend_queue_item && pToSend_queue_item->fSendImmediately )
			{
				debug("Sending packet (tag=%#x) immediately\n", pToSend_queue_item->Tag);

				pThis->send_packet_from_queue(pToSend_queue_item);
			}
		}
		
		
		TAILQ_FOREACH_SAFE(pToSend_queue_item, &pThis->m_to_send_queue, q_next, pToSend_queue_tmp)
		{
			if ( pToSend_queue_item )
			{
				//---------------------------------//
				// Slow Start / Congestion control //
				//---------------------------------//
#ifndef NO_FLOW_CONTROL
				IdleTime_us = time_since_now_us(pThis->m_pInterfaces->get_time_since_last_send(pToSend_queue_item->if_sent));
				nOutstanding = pThis->get_outstanding(pToSend_queue_item->if_sent);

				// Disable idle handling as it's hurting performance
				#if 0	
				if ( (nOutstanding==0) && (IdleTime_us > pThis->get_rto_us()) )
				{
					#ifdef DEBUG_TRANSMIT
					debugVerbose("\tInterface was idle, resetting CWND=1\n");
					#endif

					pThis->m_pInterfaces->set_cwnd(pToSend_queue_item->if_sent, 1);
					pThis->m_pInterfaces->set_ssthresh(pToSend_queue_item->if_sent, 1);
				}
				#endif

				nCWND = pThis->m_pInterfaces->get_cwnd(pToSend_queue_item->if_sent);
				nMaxForThisShelf = pThis->m_pInterfaces->get_max_outstanding(pToSend_queue_item->if_sent, pToSend_queue_item->nShelf);

				nMaxoutstanding = MIN(nCWND, nMaxForThisShelf);
				nMaxoutstanding = MIN(nMaxoutstanding,  pThis->m_pInterfaces->m_nMaxUserWindow);

				// Figure out if we can actually send this packet
				#ifdef DEBUG_TRANSMIT
				debugVerbose("\tinterface=%#x -[%d.*] -- current outstanding=%d ... nMaxoutstanding=MIN(nCWND=%d, nMaxForThisShelf=%d, UserWindow=%d)=%d [%s]\n", pToSend_queue_item->if_sent, pToSend_queue_item->nShelf, nOutstanding, nCWND, nMaxForThisShelf, pThis->m_pInterfaces->m_nMaxUserWindow, nMaxoutstanding, (nOutstanding>=nMaxoutstanding)?"NOT SENDING":"SENDING");
				#endif

				if ( 1!=pThis->m_pInterfaces->is_used(pToSend_queue_item->if_sent) )			// NOTE "is_used" returns 0 for not used, 1 for used and -1 for error
				{
					debugError("Interface is disabled and there are still packets in the send queue\n");
					
					// This shouldn't occur, but TODO: empty any outstanding packets
				}

				if ( nOutstanding>=nMaxoutstanding )
				{
					// We are too busy right now...
#ifdef DEBUG_TRANSMIT
					debugVerbose("\tnot sending packet with tag %#x\n", pToSend_queue_item->Tag);
#endif

					// Check if we should bother iterating through the rest of the loop
					if ( 0==pThis->m_pInterfaces->all_full(nMaxoutstanding) )
					{
#ifdef DEBUG_TRANSMIT
						debugVerbose("\tNo more interfaces have data to send.\n");
#endif
						break;
					}
					
					// Otherwise, keep iterating, other interfaces may have packets to send
					continue;
				}

				// Make a note if we still need to send more packets (this will call the timer again)
				if ( nOutstanding<nMaxoutstanding )
					fMoreToSend = TRUE;

#else
				// Force a check of the queue
				fMoreToSend = TRUE;
#endif	//NO_FLOW_CONTROL

				pThis->send_packet_from_queue(pToSend_queue_item);
				break;
			}
		}
		IOLockUnlock(pThis->m_pToSendQueueMutex);
	}
	else
		debugError("Unable to find AOE_KEXT_NAME\n");

	// Send again if queue is not empty and we know we are still within the window
	if ( fMoreToSend && !TAILQ_EMPTY(&pThis->m_to_send_queue) )
		pThis->enable_transmit_timer();

#ifdef DEBUG_TRANSMIT
	debugVerbose("TransmitTimer EXIT%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%!!\n");
#endif
}





/*---------------------------------------------------------------------------
 * This should only be called from the transmit timer (NOTE: the sent queue should be locked at this point)
 * It locates the packet in the sent queue and updates the time that it was actually sent. It also increases
 * the outstanding count (provided it isn't a resend)
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::send_packet_from_queue(struct ToSendPktQueue* pToSend_queue_item)
{
	struct SentPktQueue*	pSent_queue_item;
	struct SentPktQueue*	pTlqNext;
	bool					fInSentQueue;
	
	fInSentQueue = FALSE;

	//----------------------//
	// Begin sending packet //
	//----------------------//

	// Locate our tag in the sent queue so we can update the timer

	//debugVerbose("&&&&Iterating over our sent queue for tag: %#x\n", pToSend_queue_item->Tag);
	IOLockLock(m_pSentQueueMutex);
	for (pSent_queue_item = TAILQ_FIRST(&m_sent_queue); pSent_queue_item != NULL; pSent_queue_item = pTlqNext)
	{
		// get the next element pointer before we potentially corrupt it
		pTlqNext = TAILQ_NEXT(pSent_queue_item, q_next);
		
		if ( pSent_queue_item && (pSent_queue_item->Tag==pToSend_queue_item->Tag) )
		{
			fInSentQueue = TRUE;
			
			if ( !pSent_queue_item->TimeFirstSent )
			{
				clock_get_uptime(&pSent_queue_item->TimeFirstSent);
				pSent_queue_item->TimeSent = pSent_queue_item->TimeFirstSent;
			}
			else
				clock_get_uptime(&pSent_queue_item->TimeSent);
			break;
		}
	}
	
	if ( fInSentQueue )
	{
		// Provided we aren't sending the packet immediately, increment the outstanding count on the interface.
		if ( !pToSend_queue_item->fSendImmediately )
			OSIncrementAtomic(pToSend_queue_item->pOutstandingCount);
		else
			debug("\tNot incrementing outstanding count as we're sending the packet immediately\n");
		
		debugVerbose("\tOutputting packet with tag %#x on (ifnet=%#x)\n", pToSend_queue_item->Tag, pToSend_queue_item->if_sent);
		
		// Update time
		m_pInterfaces->update_time_since_last_send(pToSend_queue_item->if_sent);
		
		// Finally...we send the data...
		ifnet_output_raw(pToSend_queue_item->if_sent, PF_INET, pToSend_queue_item->mbuf);
		
		// Watch for retransmit
		enable_retransmit_timer();

		// If this isn't a broadcast, kick the idle watchdog
		if ( !(pToSend_queue_item->Tag&TAG_BROADCAST_MASK) )
			enable_idle_timer(pToSend_queue_item->if_sent);
	}
	else
	{
		debugVerbose("Transmit packet (tag=%#x) not in sent queue\n", pToSend_queue_item->Tag);
		// NOTE: A packet can only be in the send queue and not in the sent queue if a packet is retransmit after a timeout
		// and the packet is received before the actual transmit occurs
		// In this case, we dont bother sending, just remove it from the queue...

		// NOTE: Shouldn't need to free here
		//mbuf_freem(pToSend_queue_item->mbuf);
	}
	
	remove_from_queue(pToSend_queue_item);
	IOLockUnlock(m_pSentQueueMutex);
}




/*---------------------------------------------------------------------------
 * Idle timer is acting as a watchdog. It's "kicked" whenever we actually send data out an interface
 * Although the interface is passed, it isn't used at this point (each interface has a separate timer for
 * when it was last accessed)
 ---------------------------------------------------------------------------*/

void AOE_KEXT_NAME::enable_idle_timer(ifnet_t ifref)
{
	int nDelay;
	
	nDelay = IDLE_DELAY_US;
	
	// Re-start our idle timer
	if ( m_pIdleTimer->isEnabled() )
	m_pIdleTimer->disable();
	
	m_pIdleTimer->enable();
	m_pIdleTimer->setTimeoutUS(nDelay);
			
#ifdef DEBUG_IDLE
	debug("Setting idle timer with %luus delay\n", nDelay);
#endif
}


/*---------------------------------------------------------------------------
 * Reset the interfaces counters if the idle timer has expired
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::IdleTimer(OSObject* pOwner, IOTimerEventSource* pSender)
{
	AOE_KEXT_NAME*			pThis;
	
	pThis = OSDynamicCast(AOE_KEXT_NAME, pOwner);

#ifdef DEBUG_IDLE
	debug("@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
	debug("@@@@@  Interface idle @@@@@\n");
	debug("@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
#endif

	if ( pThis && pThis->m_pInterfaces )
		pThis->m_pInterfaces->reset_if_idle(IDLE_DELAY_US);
}





/*---------------------------------------------------------------------------
 * Handle any retransmissions (if necessary)
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::RetransmitTimer(OSObject* pOwner, IOTimerEventSource* pSender)
{
	AOE_KEXT_NAME*			pThis;
	struct SentPktQueue*	pSent_queue_tmp;
	struct SentPktQueue*	pSent_queue_item;
	bool					fHaveAdjustedCWND;
	
	pThis = OSDynamicCast(AOE_KEXT_NAME, pOwner);
	fHaveAdjustedCWND = FALSE;

#ifdef DEBUG_RETRANSMIT
	debug("RetransmitTimer FIRED%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%!!\n");
#endif
	
	// Allow the re-transmit timer to be called again. This is done at the beginning because resend_packet will re-arm the timer
	pSender->disable();
	
	if ( pThis )
	{
		IOLockLock(pThis->m_pSentQueueMutex);
		
		TAILQ_FOREACH_SAFE(pSent_queue_item, &pThis->m_sent_queue, q_next, pSent_queue_tmp)
		{
			if ( pSent_queue_item )
			{
				// Firelog seems to have an issue with having both of these commands on the same line.
				#ifdef DEBUG_RETRANSMIT
				if ( pSent_queue_item->TimeFirstSent && pSent_queue_item->TimeSent )
				{
					debug("\tPacket with tag %#x is %luus old  ", pSent_queue_item->Tag, time_since_now_us(pSent_queue_item->TimeSent));
					debugShort("(first sent %luus ago)\n", time_since_now_us(pSent_queue_item->TimeFirstSent));
				}
				else
				{
					//debug("\tPacket with tag %#x hasn't been sent yet...", pSent_queue_item->Tag);
					//debugShort("(first sent=%luus) ", time_since_now_us(pSent_queue_item->TimeFirstSent));
					//debugShort("(time sent=%luus)\n", time_since_now_us(pSent_queue_item->TimeSent));

					if ( TAILQ_EMPTY(&pThis->m_to_send_queue) )
						debugError("\tPacket with tag %#x hasn't been sent yet....To Send queue is empty\n", pSent_queue_item->Tag);
				}
				#endif

				// Check that the packet should be re-transmit
				if ( pSent_queue_item->RetransmitTime_us )
				{
					// NOTE: A packet is immediately placed in the sent queue prior to it being actualy sent.
					// This check ensures we dont retransmit or drop it before it's actually sent
					if ( pSent_queue_item->TimeFirstSent && pSent_queue_item->TimeSent )
					{
						if ( time_since_now_us(pSent_queue_item->TimeFirstSent) > pThis->get_max_timeout_before_drop() )
						{
							#ifdef DEBUG_RETRANSMIT
							debug("\t\tTOO LONG!! DROPPING PACKET\n");
							#endif
							pThis->remove_from_queue(pSent_queue_item);

							// Since we're dropping this packet and removing it from the queue, we decrement the number of commands outstanding
							// NOTE:	Even if we receive a response from the packet, the outstanding count will not decrement again because
							//			it's only decremented if the response is found in the sent queue

							if ( !pSent_queue_item->fPacketHasBeenRetransmit )
								OSDecrementAtomic(pSent_queue_item->pOutstandingCount);
							else
								debug("\tNot decrementing outstanding count as the packet has already been resent\n");
							debugVerbose("\tOutstanding count = %d\n", *pSent_queue_item->pOutstandingCount);
						}
						else
						{
							if ( time_since_now_us(pSent_queue_item->TimeSent) > pSent_queue_item->RetransmitTime_us )
							{
								//---------------------------------//
								// Slow Start / Congestion control //
								//---------------------------------//

								// Since we've timed out, we exponentially decrease our slow start threshold (ssthresh)
								if ( !fHaveAdjustedCWND )	// NOTE: We only adjust the window once during this timeout
								{
									int nPrevCWND = pThis->m_pInterfaces->get_cwnd(pSent_queue_item->if_sent);
									int nSSThresh = MAX(nPrevCWND/2, 1);
									pThis->m_pInterfaces->set_ssthresh(pSent_queue_item->if_sent, nSSThresh);
									pThis->m_pInterfaces->set_cwnd(pSent_queue_item->if_sent, 1);
									debugVerbose("\tAdjusting cwnd to %d and ssthresh to %d (cwnd was %d)\n", pThis->m_pInterfaces->get_cwnd(pSent_queue_item->if_sent), nSSThresh, nPrevCWND);

									fHaveAdjustedCWND = TRUE;
								}

								// During a retransmit, we still decrement the outstanding count. Retransmit packets (on reception) do NOT decrement the count
								// NOTE: We still have to make sure we dont decrement the count if the packet has already been retransmit
								if ( !pSent_queue_item->fPacketHasBeenRetransmit )
									OSDecrementAtomic(pSent_queue_item->pOutstandingCount);
								else
									debug("\tNot decrementing outstanding count as the packet has already been resent\n");

								#ifdef DEBUG_RETRANSMIT
								debug("\t\tRetransmitTime_us = %luus\n", pSent_queue_item->RetransmitTime_us);
								#endif
								IOLockUnlock(pThis->m_pSentQueueMutex);
								pThis->resend_packet(pSent_queue_item);
								IOLockLock(pThis->m_pSentQueueMutex);
							}
							else
							{
								#ifdef DEBUG_RETRANSMIT
								debug("\t\t not retransmitting yet as - RetransmitTime_us = %luus\n", pSent_queue_item->RetransmitTime_us);
								#endif
							}
						}
					}
				}
				else
				{
					#ifdef DEBUG_RETRANSMIT
					debug("\t\tPacket timed out, but doesn't require re-transmit...DROPPING PACKET\n");
					#endif
					pThis->remove_from_queue(pSent_queue_item);
					
					if ( !pSent_queue_item->fPacketHasBeenRetransmit )
						OSDecrementAtomic(pSent_queue_item->pOutstandingCount);
					else
						debug("\tNot decrementing outstanding count as the packet has already been resent\n");

					debugVerbose("\tOutstanding count = %d\n", *pSent_queue_item->pOutstandingCount);
				}
			}
		}
		
		// If the tail isn't empty, we re-enable the re-transmit timer (unless it was already armed in resend_packet).
		if ( !TAILQ_EMPTY(&pThis->m_sent_queue) )
			pThis->enable_retransmit_timer();
		
		IOLockUnlock(pThis->m_pSentQueueMutex);
	}
	else
		debugError("Unable to find AOE_KEXT_NAME\n");
	
	#ifdef DEBUG_RETRANSMIT
	debug("RetransmitTimer EXIT%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%!!\n");
	#endif
}




#pragma mark -
#pragma mark Queue handling

/*---------------------------------------------------------------------------
 * This is an interface for sending packets. Called from our controller interface
 * Additional info is passed on the function, although that sort of data is in the mbuf, it saves us searching around for it.
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::send_packet_on_interface(ifnet_t ifp, UInt32 Tag, mbuf_t m, int nShelf, bool fRetransmit /*=TRUE*/)
{
	struct SentPktQueue*	pSent_queue_item;
	struct ToSendPktQueue*	pToSend_queue_item;
	struct ToSendPktQueue*	pToSend_queue_tmp;
	struct ether_header*	eh;
	errno_t	result;

	// Before we start, check the interface is still in use...

	if ( 1!=m_pInterfaces->is_used(ifp) )		// NOTE "is_used" returns 0 for not used, 1 for used and -1 for error
	{
		// First, drop this packet
		debug("Interface is disabled. Dropping packet...\n");
		mbuf_freem(m);
		
		// Check if any additional packets have made their way on to the send queue, and if so, remove them
		IOLockLock(m_pToSendQueueMutex);
		TAILQ_FOREACH_SAFE(pToSend_queue_item, &m_to_send_queue, q_next, pToSend_queue_tmp)
		{
			if ( pToSend_queue_item && (1!=m_pInterfaces->is_used(pToSend_queue_item->if_sent)) )
			{
				debug("\tremoving additional packet from queue...\n");
				remove_from_queue(pToSend_queue_item);
			}
		}
		IOLockUnlock(m_pToSendQueueMutex);

		return -1;
	}
	
	// The mbuf will currently be pointing to the ethernet header,
	// setup the sender MAC address based on the interface we are connected to
	eh = MTOD(m,struct ether_header*);
	if ( NULL==eh )
		return -1;

	result = ifnet_lladdr_copy_bytes(ifp, eh->ether_shost, sizeof(eh->ether_shost));
	
	if (result != 0)
	{
		debugError("ifnet_lladdr_copy_bytes failed\n");
		return result;
	}

	// Before we send, we place a copy of the mbuf in our sent queue. This will be used to track dropped packets and calculate timings
	pSent_queue_item = (struct SentPktQueue*) IOMalloc(sizeof(struct SentPktQueue));
	if ( pSent_queue_item )
	{
		memset(pSent_queue_item, 0, sizeof(struct SentPktQueue));
		
		result = mbuf_dup(m, MBUF_WAITOK, &pSent_queue_item->first_mbuf);
		pSent_queue_item->Tag = Tag;
		pSent_queue_item->RetransmitTime_us = fRetransmit ? get_rto_us() : 0;
		pSent_queue_item->if_sent = ifp;
		pSent_queue_item->fPacketHasBeenRetransmit = FALSE;
		pSent_queue_item->nShelf = nShelf;
		
		// NOTE:	We keep a pointer to the outstanding count in the sent queue so we can decrement the correct count when the tag returns
		//			It may not be safe to assume that the tag will return on the same interface that it was sent on.
		pSent_queue_item->pOutstandingCount =  m_pInterfaces->get_ptr_outstanding(ifp);
		
		// Force transmit times to zero so we know to update them when the packet is actually sent
		pSent_queue_item->TimeSent = pSent_queue_item->TimeFirstSent = 0;
		
		IOLockLock(m_pSentQueueMutex);
		TAILQ_INSERT_TAIL(&m_sent_queue, pSent_queue_item, q_next);
		IOLockUnlock(m_pSentQueueMutex);

		//debugVerbose("Placing mbuf with tag %#x in out sent queue\n", Tag);
	}
	else
	{
		debugError("Error - failed to allocate memory for sent item queue.\n");
	}
	
	return add_to_send_queue(ifp, Tag, m, nShelf, pSent_queue_item->pOutstandingCount, FALSE);
}




/*---------------------------------------------------------------------------
 * Dont send the packet immediately, but place it on our queue - ready to go
 * The timer is enabled to handle the actual transmission. This allows us to exit the function and send the actual data at a later time
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::add_to_send_queue(ifnet_t ifp, UInt32 Tag, mbuf_t m, int nShelf, SInt32* pOutstandingCount, bool fSendImmediately, int nDelaySend_us /*= 0*/)
{
	struct ToSendPktQueue*	pSend_queue_item;
	
	// Before we send, we place a copy of the mbuf in our sent queue. This will be used to track dropped packets and calculate timings
	pSend_queue_item = (struct ToSendPktQueue*) IOMalloc(sizeof(struct ToSendPktQueue));
	
	if ( pSend_queue_item )
	{
		memset(pSend_queue_item, 0, sizeof(struct ToSendPktQueue));
		
		pSend_queue_item->Tag = Tag;
		pSend_queue_item->if_sent = ifp;
		pSend_queue_item->mbuf = m;
		pSend_queue_item->pOutstandingCount = pOutstandingCount;
		pSend_queue_item->nShelf = nShelf;
		pSend_queue_item->fSendImmediately = fSendImmediately;
		
		IOLockLock(m_pToSendQueueMutex);
		TAILQ_INSERT_TAIL(&m_to_send_queue, pSend_queue_item, q_next);
		IOLockUnlock(m_pToSendQueueMutex);
	}
	
	enable_transmit_timer(nDelaySend_us);
	
	return 0;
}




/*---------------------------------------------------------------------------
 * Remove sent of "to send" items from the queue. These are called after we're done with the item in the queue
 ---------------------------------------------------------------------------*/
void AOE_KEXT_NAME::remove_from_queue(struct SentPktQueue* pSent_queue_item)
{
	// It's assumed this function is called WITH the lock set
	if ( pSent_queue_item )
	{
		TAILQ_REMOVE(&m_sent_queue, pSent_queue_item, q_next);
		IOLockUnlock(m_pSentQueueMutex);
		mbuf_freem(pSent_queue_item->first_mbuf);
		IOFree(pSent_queue_item, sizeof(struct SentPktQueue));
		IOLockLock(m_pSentQueueMutex);
	}
	else
	{
		debugError("Invalid queue item\n");
	}
}

void AOE_KEXT_NAME::remove_from_queue(struct ToSendPktQueue* pToSend_queue_item)
{
	// It's assumed this function is called WITH the lock set
	if ( pToSend_queue_item )
	{
		TAILQ_REMOVE(&m_to_send_queue, pToSend_queue_item, q_next);
		IOLockUnlock(m_pToSendQueueMutex);
		IOFree(pToSend_queue_item, sizeof(struct ToSendPktQueue));
		IOLockLock(m_pToSendQueueMutex);
	}
	else
	{
		debugError("Invalid queue item\n");
	}
}




#pragma mark -
#pragma mark Error Handling

/*---------------------------------------------------------------------------
 * User interface to obtain the error info. Currently, we only return the number of unexpected responses and the number of retransmits
 ---------------------------------------------------------------------------*/
errno_t AOE_KEXT_NAME::get_error_info(ErrorInfo* pEInfo)
{
	debug("unexpected=%d\n", m_nNumUnexpectedResponses);
	debug("nRetransmits=%d\n", m_nNumRetransmits);
	
	pEInfo->nUnexpectedResponses = m_nNumUnexpectedResponses;
	pEInfo->nRetransmits = m_nNumRetransmits;
	
	return 0;
}




#pragma mark -
#pragma mark C interface functions

/*---------------------------------------------------------------------------
 * This is called from the BSD-style C functions that handle the lower level interface filtering on the ethernet packets
 * This function is the bridge between the C and C++ worlds...
 ---------------------------------------------------------------------------*/
extern "C" int c_aoe_incoming(void* pController, ifnet_t ifp, struct ether_header* pEHeader, mbuf_t* pMBufData)
{
	int nRet = -1;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		nRet = pAoEService->aoe_incoming(ifp, pEHeader, pMBufData);
	else
		debugError("Controller not defined\n");
	
	return nRet;
}



extern "C" void c_set_logging(void* pController, int nLoggingLevel)
{
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		pAoEService->m_nLoggingLevel = nLoggingLevel;
	else
		debugError("Controller not defined\n");
}

extern "C" int c_get_logging(void* pController)
{
	int nLevel = 0;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		nLevel = pAoEService->m_nLoggingLevel;
	else
		debugError("Controller not defined\n");
	
	return nLevel;
}

extern "C" kern_return_t c_enable_interface(void* pController, int nEthernetNumber)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->enable_interface(nEthernetNumber);
	else
		debugError("Controller not defined\n");
	
	return retval;
}

extern "C" kern_return_t c_disable_interface(void* pController, int nEthernetNumber)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->disable_interface(nEthernetNumber);
	else
		debugError("Controller not defined\n");
	
	return retval;
}

extern "C" void c_interface_disconnected(void* pController, int nEthernetNumber)
{
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		pAoEService->interface_disconnected(nEthernetNumber);
	else
		debugError("Controller not defined\n");
}

extern "C" void c_interface_reconnected(void* pController, int nEthernetNumber, ifnet_t enetifnet)
{
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		pAoEService->interface_reconnected(nEthernetNumber, enetifnet);
	else
		debugError("Controller not defined\n");
}

extern "C" int c_update_target(void* pController, int* pnNumberOfTargets)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->find_targets(pnNumberOfTargets);
	else
		debugError("Controller not defined\n");
	
	return retval;
}

extern "C" int c_get_target_info(void* pController, int nDevice, TargetInfo* pTargetData)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->get_target_info(nDevice, pTargetData);
	else
		debugError("Controller not defined\n");
	
	return retval;
}

extern "C" int c_get_error_info(void* pController, ErrorInfo* pEInfo)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->get_error_info(pEInfo);
	else
		debugError("Controller not defined\n");
	
	return retval;	
}

extern "C" int c_get_payload_size(void* pController, UInt32* pPayloadSize)
{
	kern_return_t	retval = KERN_FAILURE;

	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->get_payload_size(pPayloadSize);
	else
		debugError("Controller not defined\n");
	
	return retval;	
}

extern "C" int c_force_packet(void* pController, ForcePacketInfo* pForcedPacketInfo)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->force_packet(pForcedPacketInfo);
	else
		debugError("Controller not defined\n");
			
	return retval;	
}

extern "C" int c_set_targets_cstring(void* pController, ConfigString* pCStringInfo)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->set_targets_cstring(pCStringInfo);
	else
		debugError("Controller not defined\n");
	
	return retval;
}

extern "C" int c_set_ourcstring(void* pController, char* pszCStringInfo)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->set_our_cstring(pszCStringInfo);
	else
		debugError("Controller not defined\n");
			
	return retval;
}

extern "C" int c_set_max_transfer_size(void* pController, int nMaxSize)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->set_max_transfer_size(nMaxSize);
	else
		debugError("Controller not defined\n");
	
	return retval;
}


extern "C" int c_set_user_window(void* pController, int nMaxSize)
{
	kern_return_t	retval = KERN_FAILURE;
	
	AOE_KEXT_NAME* pAoEService = (AOE_KEXT_NAME*) pController;
	if ( pAoEService )
		retval = pAoEService->set_user_window(nMaxSize);
	else
		debugError("Controller not defined\n");
	
	return retval;
}



