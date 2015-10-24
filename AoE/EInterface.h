/*
 *  EInterface.h
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#ifndef __EINTERFACE_H__
#define __EINTERFACE_H__

#include <sys/kernel_types.h>
#include <sys/types.h>
#include "aoe.h"

class EInterface
{
public:
	EInterface();
	~EInterface();

	void set_max_oustanding(int nShelf, int nMaxOutstanding);
	int get_max_oustanding(int nShelf);
	int get_max_outstanding_all_shelves(void);

public:
	bool		m_fEnabled;
	ifnet_t		m_ifnet;
	SInt32		m_nOutstandingCount;
	UInt32		m_nSSThresh;
	UInt32		m_nCwd;
	UInt32		m_nCwdFractional;
	
	uint64_t	m_TimeSinceLastSend;

private:
	UInt32		m_anMaxOutstanding[MAX_SHELFS];
	UInt32		m_nMinimumMaxOutstanding;
};

#endif		//__EINTERFACE_H__
