/*
 *  EInterface.cpp
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <IOKit/IOLib.h>
#include <string.h>
#include "EInterface.h"
#include "../Shared/AoEcommon.h"

EInterface::EInterface()
{
	m_fEnabled = FALSE;
	m_ifnet = 0;
	m_nOutstandingCount = 0;
	m_nCwd = 1;
	m_nCwdFractional = 0;
	m_TimeSinceLastSend = 0;
	m_nMinimumMaxOutstanding = DEFAULT_CONGESTION_WINDOW;
	m_nSSThresh = m_nMinimumMaxOutstanding/2;

	memset(m_anMaxOutstanding, 0, sizeof(m_anMaxOutstanding));
}


EInterface::~EInterface()
{
}

#warning use OSArray or something to avoid pre-allocating all this memory
void EInterface::set_max_oustanding(int nShelf, int nMaxOutstanding)
{
	m_anMaxOutstanding[nShelf] = nMaxOutstanding;
	
	// Update the minimum value for all shelves
	m_nMinimumMaxOutstanding = MIN(m_nMinimumMaxOutstanding, nMaxOutstanding);
}

int EInterface::get_max_oustanding(int nShelf)
{
	return m_anMaxOutstanding[nShelf];
}

int EInterface::get_max_outstanding_all_shelves(void)
{
	return m_nMinimumMaxOutstanding;
}
