/*
 *  EInterfaces.h
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#ifndef __EINTERFACES_H__
#define __EINTERFACES_H__

#include <sys/kernel_types.h>
#include <sys/types.h>
#include "EInterface.h"
#include "aoe.h"
#include "../Shared/AoEcommon.h"

class EInterfaces
{
public:
	EInterfaces(IOService* pProvider);
	~EInterfaces();

	int get_outstanding(ifnet_t ifref);
	int set_outstanding(ifnet_t ifref, int nOutstanding);
	void set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding);
	int get_max_outstanding(ifnet_t ifref, int nShelf);
	int get_cwnd(ifnet_t ifref);	
	int set_cwnd(ifnet_t ifref, int nCwd);
	int grow_cwnd(ifnet_t ifref, int nIntegerGrowth, int nFractionalGrowth);
	int get_ssthresh(ifnet_t ifref);
	int set_ssthresh(ifnet_t ifref, int nSsthresh);

	int update_time_since_last_send(ifnet_t ifref);
	uint64_t get_time_since_last_send(ifnet_t ifref);

	kern_return_t enable_interface(int nEthernetNumber);

	void interface_reconnected(int nEthernetNumber, ifnet_t enetifnet);
	int interface_disconnected(int nEthernetNumber);
	SInt32* get_ptr_outstanding(ifnet_t ifref);
	ifnet_t get_nth_interface(int n);

	int set_user_max_window(int nMaxSize);
	int all_full(int nMax);
	int is_used(ifnet_t ifref);
	
	int reset_if_idle(UInt64 TimeOut);

	int get_mtu(void)	{ return m_Min_MTU; };

	int					m_nMaxUserWindow;
private:
	void update_interface_property(void);
	void recalculate_mtu(void);
	
	EInterface			m_aInterfaces[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	int					m_nInterfacesInUse;

	UInt32				m_Min_MTU;
	IOService*			m_pProvider;
};

#endif		//__EINTERFACES_H__
