/*
 *  AoEControllerInterface.h
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#ifndef __AOE_CONTROLLER_INTERFACE_H__
#define __AOE_CONTROLLER_INTERFACE_H__

#include <IOKit/IOService.h>
#include "../Shared/AoEcommon.h"
#include "aoe.h"

class AOE_KEXT_NAME;
class AOE_DEVICE_NAME;
class AOE_CONTROLLER_NAME;
class OSArray;

class AOE_CONTROLLER_INTERFACE_NAME : public IOService
{
	OSDeclareDefaultStructors(AOE_CONTROLLER_INTERFACE_NAME);
	
public:
	bool init(AOE_KEXT_NAME* pAoEService);
	void uninit(void);
	
	int aoe_config_receive(ifnet_t ifnet_receive, struct ether_header* pEHeader, aoe_header* pAoEFullHeader, aoe_cfghdr_rd* pCfgHeader, mbuf_t* pMBufData);
	int aoe_ata_receive(aoe_header* pAoEFullHeader, aoe_atahdr_rd* pATAHeader, mbuf_t* pMBufData);
	int force_packet_send(ForcePacketInfo* pForcedPacketInfo);

	void check_down_targets(void);
	UInt32	next_tag();

	void start_lun_search(bool fRun);
	int	number_of_targets(void);
	TargetInfo* get_target_info(int nNumber);
	int set_targets_cstring(int nDevice, const char* pszConfigString, int nLength);
	
	int send_ata_packet(AOE_CONTROLLER_NAME* pSender, mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo);
	int send_aoe_packet(AOE_CONTROLLER_NAME* pSender, mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo);
	
	void set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding);
	void set_max_transfer_size(int nMaxTransferSize);
	int remove_target(int nNumber);

	void fake_device_attach(void);
	errno_t aoe_search(ifnet_t ifnet);
	int cancel_commands_on_interface(ifnet_t enetifnet);
	void adjust_mtu_sizes(int nMTU);
	bool interfaces_active(TargetInfo* pTargetInfo);
	int get_next_target_number(void);
	void reenable_controllers(void);
	void identify_all_targets();

private:
	static void StateUpdateTimer(OSObject *owner, IOTimerEventSource *sender);
	static void FakeReturnTimer(OSObject *owner, IOTimerEventSource *sender);
	int send_packet(mbuf_t m, UInt32 Tag, TargetInfo* pTargetInfo);

	OSArray*						m_pControllers;
	IOTimerEventSource*				m_pStateUpdateTimer;
	IOTimerEventSource*				m_pFakeReturnTimer;
	bool							m_fLUNSearchRunning;
	IOLock*							m_pTargetListMutex;
	UInt64							m_TimeUntilTargetOffline_us;
	AOE_CONTROLLER_NAME*			m_pControllerToFakeResponse;
	UInt32							m_nCurrentTag;
	AOE_KEXT_NAME*					m_pAoEService;
	int								m_nMaxTransferSize;
};

#endif	//__AOE_CONTROLLER_INTERFACE_H__
