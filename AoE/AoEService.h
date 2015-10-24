//
//  AoEFilterService.h
//  AoEInterface
//
//  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
//

#ifndef __AOESERVICE_H__
#define __AOESERVICE_H__

#include <net/ethernet.h>
#include <kern/kern_types.h>
#include "../Shared/AoEcommon.h"
#include "aoe.h"

#ifdef __cplusplus
#include <IOKit/IOService.h>
#include <net/ethernet.h>
#include <sys/queue.h>
#include "EInterfaces.h"

// SentPktQueue is the structure which describes the packets sent (or about to be sent) on a particular interface
struct SentPktQueue
{
	TAILQ_ENTRY(SentPktQueue)	q_next;		// queued entries
	mbuf_t						first_mbuf;
	ifnet_t						if_sent;
	uint64_t					TimeSent;
	uint64_t					TimeFirstSent;
	uint64_t					RetransmitTime_us;
	UInt32						Tag;
	UInt32						nShelf;
	bool						fPacketHasBeenRetransmit;

	SInt32*						pOutstandingCount;
};

// ToSendPktQueue is the structure which describes the packets soon to be sent on a particular interface
// NOTE:	When an item is in the Send queue, it is also actually in the Sent queue.
//			When the packet is actually sent, it updates the sent queue values: TimeSent and TimeFirstSent.
struct ToSendPktQueue
{
	TAILQ_ENTRY(ToSendPktQueue)	q_next;		// queued entries
	mbuf_t						mbuf;
	ifnet_t						if_sent;
	UInt32						Tag;
	UInt32						nShelf;
	UInt32						fSendImmediately;

	SInt32*						pOutstandingCount;
};


TAILQ_HEAD(SentPktQueueHeadStruct, SentPktQueue);
TAILQ_HEAD(ToSendPktQueueHeadStruct, ToSendPktQueue);

class AOE_CONTROLLER_INTERFACE_NAME;

class AOE_KEXT_NAME : public IOService
{
	OSDeclareDefaultStructors(AOE_KEXT_NAME)
public:
	virtual bool init(OSDictionary *dictionary = 0);
	virtual void free(void);
	virtual IOService *probe(IOService *provider, SInt32 *score);
	virtual bool start(IOService *provider);
	virtual void stop(IOService *provider);
	
	kern_return_t enable_interface(int nEthernetNumber);
	kern_return_t disable_interface(int nEthernetNumber);
	
	errno_t find_targets(int* pnTargets);
	errno_t get_target_info(int nDevice, TargetInfo* pTargetData);
	errno_t set_targets_cstring(ConfigString* CStringInfo);
	
	// Flow control
	errno_t send_packet_on_interface(ifnet_t ifp, UInt32 Tag, mbuf_t m, int nShelf, bool fRetransmit = TRUE);
	void resend_packet(struct SentPktQueue* pSent_queue_item);
	void update_rto(uint64_t rtt);
	UInt64 get_rto_us(void);
	UInt64 get_max_timeout_before_drop(void);
	
	// General helper functions
	void remove_from_queue(struct SentPktQueue* pSent_queue_item);
	void remove_from_queue(struct ToSendPktQueue* pToSend_queue_item);

	int get_outstanding(ifnet_t ifref);
	void set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding);
	const char* get_com_cstring(void) { return m_pszOurCString; };
	
	// Only called by c_functions
	int aoe_incoming(ifnet_t ifp, struct ether_header* pEHeader, mbuf_t* pMBufData);
	void interface_reconnected(int nEthernetNumber, ifnet_t enetifnet);
	void interface_disconnected(int nEthernetNumber);
	errno_t get_error_info(ErrorInfo* pEInfo);
	UInt32 get_mtu(void);
	UInt32 get_sector_count(void);
	int get_payload_size(UInt32* pPayloadSize);
	int force_packet(ForcePacketInfo* pForcedPacketInfo);
	errno_t aoe_search_all(void);

	int set_our_cstring(const char* pszOurCString);
	int set_max_transfer_size(int nMaxSize);
	int set_user_window(int nMaxSize);
	void send_packet_from_queue(struct ToSendPktQueue* pToSend_queue_item);
	bool interfaces_active(TargetInfo* pTargetInfo);
	bool interface_active(TargetInfo* pTargetInfo, int nInterfaceNumber);
public:
	int								m_nLoggingLevel;
private:
	static void RetransmitTimer(OSObject* pOwner, IOTimerEventSource* pSender);
	static void TransmitTimer(OSObject* pOwner, IOTimerEventSource* pSender);
	static void IdleTimer(OSObject* pOwner, IOTimerEventSource* pSender);

	static void cg_aoe_incoming(OSObject* owner, void* arg0, void* arg1, void*   arg2, void* /*arg3*/);
	static void cg_force_packet(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/);
	static void cg_set_targets_cstring(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/);
	static void cg_enable_interface(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/);
	static void cg_disable_interface(OSObject* owner, void* arg0, void* arg1, void* arg2, void* /*arg3*/);
	void enable_retransmit_timer(void);
	void enable_idle_timer(ifnet_t ifref);
	void enable_transmit_timer(int nDelaySend_us = 3);
	errno_t add_to_send_queue(ifnet_t ifp, UInt32 Tag, mbuf_t m, int nShelf, SInt32* pOutstandingCount, bool fSendImmediately, int nDelaySend_us = 0);

	char*							m_pszOurCString;
	EInterfaces*					m_pInterfaces;
	IOService*						m_pAoEService;
	AOE_CONTROLLER_INTERFACE_NAME*	m_pAoEControllerInterface;
	struct SentPktQueueHeadStruct	m_sent_queue;
	struct ToSendPktQueueHeadStruct	m_to_send_queue;
	IOLock*							m_pSentQueueMutex;
	IOLock*							m_pToSendQueueMutex;
	IOLock*							m_pGeneralMutex;
	UInt64							m_nRTO_ns;
	int								m_nScaledRTTavg;
	int								m_nScaledRTTvar;
	UInt64							m_MaxTimeOutBeforeDrop;
	IOTimerEventSource*				m_pRetransmitTimer;
	IOTimerEventSource*				m_pTransmitTimer;
	IOTimerEventSource*				m_pIdleTimer;
	IOCommandGate*					m_pCmdGate;

	int								m_nNumUnexpectedResponses;
	int								m_nNumRetransmits;
};
#endif


#ifndef __cplusplus
// Externally accessable "C" functions
__private_extern__ int c_aoe_incoming(void* pController, ifnet_t ifp, struct ether_header* pEHeader, mbuf_t* pMBufData);
__private_extern__ void c_set_logging(void* pController, int nLoggingLevel);
__private_extern__ int c_get_logging(void* pController);
__private_extern__ kern_return_t c_enable_interface(void* pController, int nEthernetNumber);
__private_extern__ kern_return_t c_disable_interface(void* pController, int nEthernetNumber);
__private_extern__ void c_interface_disconnected(void* pController, int nEthernetNumber);
__private_extern__ void c_interface_reconnected(void* pController, int nEthernetNumber, ifnet_t enetifnet);
__private_extern__ int c_update_target(void* pController, int* pnNumberOfTargets);
__private_extern__ int c_get_target_info(void* pController, int nDevice, TargetInfo* pTargetData);
__private_extern__ int c_get_error_info(void* pController, ErrorInfo* pEInfo);
__private_extern__ int c_get_payload_size(void* pController, UInt32* pPayloadSize);
__private_extern__ int c_force_packet(void* pController, ForcePacketInfo* pForcedPacketInfo);
__private_extern__ int c_set_targets_cstring(void* pController, ConfigString* pCStringInfo);
__private_extern__ int c_set_ourcstring(void* pController, char* pszCStringInfo);
__private_extern__ int c_set_max_transfer_size(void* pController, int nMaxSize);
__private_extern__ int c_set_user_window(void* pController, int nMaxSize);

#endif

#endif //__AOESERVICE_H__


