/*
 *  AoEController.h
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_CONTROLLER_H__
#define __AOE_CONTROLLER_H__

#include <IOKit/IOService.h>
#include <sys/queue.h>
#include <IOKit/ata/IOATATypes.h>
#include <IOKit/ata/IOATAController.h>
#include "../Shared/AoEcommon.h"
#include "aoe.h"

class AOE_DEVICE_NAME;
class AOE_CONTROLLER_INTERFACE_NAME;
class IOExtendedLBA;


//class AOE_CONTROLLER_NAME : public IOATAController
class AOE_CONTROLLER_NAME : public IOATAController
{
	OSDeclareDefaultStructors(AOE_CONTROLLER_NAME);

public:
	bool init(AOE_CONTROLLER_INTERFACE_NAME* pProvider, int nShelf, int nSlot, ifnet_t ifnet_receive, u_char* pTargetsMACAddress, UInt32 MTU, int m_nMaxTransferSize, int nNumber);
	void uninit(void);


	void registerDiskService(void);
	void attach_device(void);
	int is_device(int nShelf, int nSlot);
	int update_target_info(ifnet_t ifnet_receive, u_char* pTargetsMACAddress, bool fOnline);
	int ata_response(aoe_atahdr_rd* pATAHeader, mbuf_t* pMBufData, UInt32 Tag);
	int target_number(void);
	void remove_all_interfaces(void);
	void set_number_sectors(UInt64 Sectors);
	TargetInfo* get_target_info(void);
	void SetLBAExtendedSupport(bool f);
	void handle_aoe_cmd(ifnet_t ifnet_receive, aoe_cfghdr_rd* pCfgHeader, mbuf_t* pMBufData);
	int set_config_string(const char* pszString, int nLength);
	uint64_t time_since_last_comm(void)	{ return m_time_since_last_comm; };
	int cstring_is_ours(const char* pszString);
	int device_attached(void);
	
	int force_packet_send(ForcePacketInfo* pForcedPacketInfo);
	int is_registered(void) { return m_fRegistered ? 0 : -1; };
	int connected_to_interface(ifnet_t enetifnet);
	void set_mtu_size(int nMTU);
	void device_online(void);
	int send_identify(void);
	bool handle_identify(aoe_atahdr_rd* pATAHeader);
	void cancel_command(bool fClean);

#if 0
	// These can be useful for debugging retain/release counts
	virtual void retain() const;
	virtual void release(int when) const;
	virtual void taggedRetain(const void *tag = 0) const;
	virtual void taggedRelease(const void *tag, const int when) const;
#endif
private:
	void remove_interface(int nInterfaceNumber);
	int create_mbuf_for_transfer(mbuf_t* m, UInt32 Tag, bool fATA);
	void print_mem(UInt8* pMem, int nSize);
	int append_write_data(mbuf_t* pm);
	static void cluster_free(caddr_t add, u_int size, caddr_t add2);
	bool is_extended_command(void);
	void increment_address(IOExtendedLBA* extLBA, int nInc);
	void increment_address(ataTaskFile* tfRegs, int nInc);
	void update_interface_property(void);
	int attach_ext_to_mbuf(mbuf_t* pm, caddr_t MBufExtData, IOByteCount Size);


	AOE_DEVICE_NAME*				m_pAoEDevice;
	AOE_CONTROLLER_INTERFACE_NAME*	m_pProvider;
	TargetInfo						m_target;
	UInt32							m_MTU;
	int								m_nMaxSectorsPerTransfer;
	aoe_atahdr_rd*					m_pReceivedATAHeader;
	UInt32							m_unReceivedATADataSize;
	bool							m_fExtendedLBA;
	int								m_nReadWriteRepliesRequired;
	UInt32							m_unReadBaseTag;
	UInt32							m_unReceivedTag;
	UInt8							m_PreviousWriteStatus;
	mbuf_t							m_ReceivedMBufCont;
	UInt8							m_PreviousWriteError;
	char							m_aConfigString[MAX_CONFIG_STRING_LENGTH];
	int								m_nBufferCount;
	uint64_t						m_time_since_last_comm;
	int								m_nMaxTransferSize;
	bool							m_fRegistered;
	ataEventCode					m_ATAState;
	UInt32							m_nOutstandingIdentTag;
	UInt64							m_IdentifiedCapacity;
	
	//-------------------------------------------------------------//
	// The following functions are overrides from IOATAController. //
	//-------------------------------------------------------------//
public:
	virtual IOReturn provideBusInfo( IOATABusInfo* infoOut);
	virtual IOReturn selectConfig( IOATADevConfig* configRequest, UInt32 unitNumber);
	virtual IOReturn getConfig( IOATADevConfig* configRequest, UInt32 unitNumber);

protected:
	// the pointers to the ATA task file registers during start() time.
	virtual bool configureTFPointers(void);
	virtual UInt32 scanForDrives( void );
	IOReturn handleBusReset(void);
	IOReturn softResetBus( bool doATAPI );
	bool waitForU8Status (UInt8 mask, UInt8 value);
	virtual bool ATAPISlaveExists( void );
	
	virtual IOReturn issueCommand( void );
	virtual IOReturn writePacket( void );
	
	virtual IOReturn startDMA( void );
	virtual IOReturn stopDMA( void );
	
	virtual IOReturn asyncData(void);
	IOReturn completeDataRead(bool* pfInterrupt);
	virtual IOReturn asyncStatus(void);
	virtual IOReturn asyncCommand(void);
	//virtual IOReturn synchronousIO(void);	
	
	virtual IOReturn registerAccess(bool isWrite);
	virtual UInt16 readExtRegister( IOATARegPtr8 inRegister );
	virtual void writeExtRegister( IOATARegPtr8 inRegister, UInt16 inValue );
	virtual IOReturn selectDevice( ataUnitID unit );
	virtual IOReturn handleDeviceInterrupt( void );
	virtual IOReturn synchronousIO(void);
	virtual IOReturn handleRegAccess( void );
	virtual void handleTimeout( void );
	virtual bool allocateDoubleBuffer( void );
	virtual bool busCanDispatch( void );
	
/////////// THE FOLLOWING OVERIDES ARE JUST FOR DEBUGGING PURPOSES
#ifdef DEBUGBUILD
protected:
	virtual IOReturn executeCommand(IOATADevice* nub, IOATABusCommand* command);
    virtual IOReturn handleCommand( void *     command,
								   void *     param1 = 0,
								   void *     param2 = 0,
								   void *     param3 = 0);
	virtual IOReturn enqueueCommand( IOATABusCommand* command);
	virtual IOATABusCommand* dequeueFirstCommand( void );
	virtual IOReturn dispatchNext( void );
	virtual void executeEventCallouts( ataEventCode event, ataUnitID unit );
	virtual void completeIO( IOReturn commandResult );
	virtual IOReturn startTimer( UInt32 inMS);
	virtual void stopTimer(void);
	virtual IOReturn handleExecIO( void );
	virtual IOReturn asyncIO(void);
	virtual IOReturn txDataIn (IOLogicalAddress buf, IOByteCount length);
	virtual IOReturn txDataOut(IOLogicalAddress buf, IOByteCount length);
	virtual IOByteCount	readATAPIByteCount (void);
	virtual transState determineATAPIState(void);
	virtual void handleOverrun( IOByteCount length);
#endif
};

#endif	//__AOE_CONTROLLER_H__
