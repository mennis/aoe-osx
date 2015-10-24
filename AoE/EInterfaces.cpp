/*
 *  EInterfaces.cpp
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <string.h>
#include "AoEtherFilter.h"
#include "EInterfaces.h"
#include "debug.h"
#include "../Shared/AoEcommon.h"


__BEGIN_DECLS
#include <net/kpi_interface.h>
__END_DECLS

#pragma mark -
#pragma mark Initialisation

EInterfaces::EInterfaces(IOService *pProvider)
{
	m_nInterfacesInUse = 0;
	m_Min_MTU = 0;
	m_pProvider = pProvider;
	m_nMaxUserWindow = DEFAULT_CONGESTION_WINDOW;
}


EInterfaces::~EInterfaces()
{
	int n;

	for(n=0; n<numberof(m_aInterfaces); n++)
	{
		if ( m_aInterfaces[n].m_fEnabled )
			ifnet_release(m_aInterfaces[n].m_ifnet);
		m_aInterfaces[n].m_fEnabled = FALSE;
	}
}

#pragma mark -
#pragma mark set/get

/*---------------------------------------------------------------------------
 * Most of these functions are passed a particular ifref and we have to search for the interface to get/set the appropriate info
 ---------------------------------------------------------------------------*/

void EInterfaces::set_max_outstanding(ifnet_t ifref, int nShelf, int nMaxOutstanding)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			m_aInterfaces[n].set_max_oustanding(nShelf, nMaxOutstanding);
}

int EInterfaces::get_max_outstanding(ifnet_t ifref, int nShelf)
{
	int n;
	
	if ( nShelf>=0 )
	{
		// Iterate over all our interfaces looking for ifref
		for(n=0; n<numberof(m_aInterfaces); n++)
			if ( ifref==m_aInterfaces[n].m_ifnet )
				return m_aInterfaces[n].get_max_oustanding(nShelf);	
	}
	else
	{
		// If nShelf<0, it's a broadcast, so we take the min of all shelves for that interface
		for(n=0; n<numberof(m_aInterfaces); n++)
			if ( ifref==m_aInterfaces[n].m_ifnet )
				return m_aInterfaces[n].get_max_outstanding_all_shelves();
	}
	
	return 0;
}

int EInterfaces::is_used(ifnet_t ifref)
{
	int n;

	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			return m_aInterfaces[n].m_fEnabled;

	return -1;
}

int EInterfaces::get_outstanding(ifnet_t ifref)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			return m_aInterfaces[n].m_nOutstandingCount;
	
	return -1;
}

int EInterfaces::get_cwnd(ifnet_t ifref)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			return m_aInterfaces[n].m_nCwd;
	
	return -1;
}

uint64_t EInterfaces::get_time_since_last_send(ifnet_t ifref)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			return m_aInterfaces[n].m_TimeSinceLastSend;
	
	return 0;
}

int EInterfaces::update_time_since_last_send(ifnet_t ifref)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
		{
			clock_get_uptime(&m_aInterfaces[n].m_TimeSinceLastSend);
			return 0;
		}
	
	return -1;
}


/*---------------------------------------------------------------------------
 * Force a cwnd value
 ---------------------------------------------------------------------------*/
int EInterfaces::set_cwnd(ifnet_t ifref, int nCwd)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
		{
			m_aInterfaces[n].m_nCwd = nCwd;
			m_aInterfaces[n].m_nCwdFractional = 0;
			return 0;
		}
	
	return -1;
}


/*---------------------------------------------------------------------------
 * Adjust the cwnd and handle any fractional component
 ---------------------------------------------------------------------------*/
int EInterfaces::grow_cwnd(ifnet_t ifref, int nIntegerGrowth, int nFractionalGrowth)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
		{
			int nPrevCwnd = m_aInterfaces[n].m_nCwd;
			int nPrevCwdFractional = m_aInterfaces[n].m_nCwdFractional;

			m_aInterfaces[n].m_nCwd += nIntegerGrowth;
			m_aInterfaces[n].m_nCwdFractional += nFractionalGrowth;

			// Check if fractional part has "rolled over"
			if ( m_aInterfaces[n].m_nCwdFractional >= m_aInterfaces[n].m_nCwd )
			{
				m_aInterfaces[n].m_nCwd += m_aInterfaces[n].m_nCwdFractional/nPrevCwnd;
				m_aInterfaces[n].m_nCwdFractional = m_aInterfaces[n].m_nCwdFractional % nPrevCwnd;
			}

			debug("\tcwnd=%d.%d + %d.%d = %d.%d\n", nPrevCwnd, nPrevCwdFractional, nIntegerGrowth, nFractionalGrowth, m_aInterfaces[n].m_nCwd, m_aInterfaces[n].m_nCwdFractional);
			return 0;
		}
	
	return -1;
}

int EInterfaces::get_ssthresh(ifnet_t ifref)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
			return m_aInterfaces[n].m_nSSThresh;
	
	return -1;
}

int EInterfaces::set_ssthresh(ifnet_t ifref, int nSSThresh)
{
	int n;
	
	// Iterate over all our interfaces looking for ifref
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( ifref==m_aInterfaces[n].m_ifnet )
		{
			m_aInterfaces[n].m_nSSThresh = nSSThresh;
			return 0;
		}
	
	return -1;
}

/*---------------------------------------------------------------------------
 * Check if the outstanding count on all enabled interfaces has been reached
 ---------------------------------------------------------------------------*/
int EInterfaces::all_full(int nMax)
{
	int n;
	int nRet = 0;
	
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( m_aInterfaces[n].m_fEnabled )
		{
			if ( m_aInterfaces[n].m_nOutstandingCount < nMax )
			{
				nRet = -1;
				break;
			}
		}

	return nRet;
}


SInt32* EInterfaces::get_ptr_outstanding(ifnet_t ifref)
{
	int nInterfaceNum;
	
	// Find which interface this belongs to
	for(nInterfaceNum=0; nInterfaceNum<numberof(m_aInterfaces); nInterfaceNum++)
		if ( m_aInterfaces[nInterfaceNum].m_ifnet == ifref )
			break;

	if ( nInterfaceNum>numberof(m_aInterfaces) )
	{
		debugError("Interface not in our list\n");
		return NULL;
	}

	return &m_aInterfaces[nInterfaceNum].m_nOutstandingCount;
}

ifnet_t EInterfaces::get_nth_interface(int n)
{
	for(/**/; n<numberof(m_aInterfaces); n++)
		if ( m_aInterfaces[n].m_fEnabled )
			return m_aInterfaces[n].m_ifnet;

	return NULL;
}



/*---------------------------------------------------------------------------
 * Handles the interface reset in the case of an idle.
 ---------------------------------------------------------------------------*/
int EInterfaces::reset_if_idle(UInt64 TimeOut)
{
	int n;
	int nRet = 0;
	
	for(n=0; n<numberof(m_aInterfaces); n++)
		if ( m_aInterfaces[n].m_fEnabled && (m_aInterfaces[n].m_TimeSinceLastSend!=0) )
		{
			debug("Interface[%d] - time since idle=%luus\n", n,  time_since_now_us(m_aInterfaces[n].m_TimeSinceLastSend));

			// Check if our interface has actually timed out
			if ( time_since_now_us(m_aInterfaces[n].m_TimeSinceLastSend) > TimeOut )
			{
				debug("RESETTING IDLE LINK on interface %d\n", n);

				// Reset values
				set_cwnd(m_aInterfaces[n].m_ifnet, 1);
				set_ssthresh(m_aInterfaces[n].m_ifnet, m_aInterfaces[n].get_max_outstanding_all_shelves()/2);
				
				// Since the link is idle, we would expect the number of outstanding commands to be zero. If it isn't
				// something has gone wrong and we reset it to prevent commands not being sent again
				if ( 0!=m_aInterfaces[n].m_nOutstandingCount )
				{
					debugError("Outstanding count is not zero, but the interface is idle. Resetting to prevent deadlock\n");
					m_aInterfaces[n].m_nOutstandingCount = 0;
				}
			}
		}

	
	return nRet;
}





#pragma mark -
#pragma mark Enable/Disable


/*---------------------------------------------------------------------------
 * Enable the interface. There is some handling to ensure we use the correct MTU size based on all the interfaces that are enabled.
 * We have to consider the case where the MTU value is not consistant across interfaces. In this case we use the minimum
 ---------------------------------------------------------------------------*/
kern_return_t EInterfaces::enable_interface(int nEthernetNumber)
{
	char			acBSDName[20];
	kern_return_t	retval;
	ifnet_t			enetifnet;
	
	if ( nEthernetNumber>=MAX_SUPPORTED_ETHERNET_CONNETIONS )
	{
		debugError("Invalid ethernet port\n");
		return -1;
	}

	debug("AOE_KEXT_NAME::enable_interface(port=%d)\n", nEthernetNumber);

	// Create our BSD name to locate ifnet_t
	snprintf(acBSDName, sizeof(acBSDName), "en%d", nEthernetNumber);

	retval = ifnet_find_by_name(acBSDName, &enetifnet);
	if (retval == KERN_SUCCESS)
		enable_filtering(nEthernetNumber, enetifnet);

	// Incremement the number of interfaces in use if it isn't already being used
	if ( !m_aInterfaces[nEthernetNumber].m_fEnabled )
		++m_nInterfacesInUse;

	m_aInterfaces[nEthernetNumber].m_ifnet = enetifnet;
	m_aInterfaces[nEthernetNumber].m_fEnabled = TRUE;
	
	// Reset our CC/SS parameters
	set_cwnd(m_aInterfaces[nEthernetNumber].m_ifnet, 1);
	set_ssthresh(m_aInterfaces[nEthernetNumber].m_ifnet, m_aInterfaces[nEthernetNumber].get_max_outstanding_all_shelves()/2);
	m_aInterfaces[nEthernetNumber].m_nOutstandingCount = 0;

	debug("enable_interface(%d), %d interface(s) now in use\n", nEthernetNumber, m_nInterfacesInUse);

	// Check the packet size based on the MTU of all the connected interfaces
	recalculate_mtu();

	// Set the properties to show the available interfaces
	update_interface_property();

	return retval;
}


/*---------------------------------------------------------------------------
 * Recheck MTU sizes basedon all the connected interfaces
 ---------------------------------------------------------------------------*/

void EInterfaces::recalculate_mtu(void)
{
	UInt32	Min_MTU;
	int		n;

	Min_MTU = 0;
	
	for(n=0; n<numberof(m_aInterfaces); n++)
	{
		if ( m_aInterfaces[n].m_fEnabled )
		{
			if ( Min_MTU )
				Min_MTU = MIN(ifnet_mtu(m_aInterfaces[n].m_ifnet), Min_MTU);
			else
				Min_MTU = ifnet_mtu(m_aInterfaces[n].m_ifnet);
		}
	}
	
	if ( 0==Min_MTU )
	{
		debugError("Error in MTU calculation.\n");
		m_Min_MTU = 1500;		// Set to standard MTU size
	}
	
	m_Min_MTU = Min_MTU;
	
	debug("Minimum MTU of %d interface(s) is %d bytes\n", m_nInterfacesInUse, m_Min_MTU);
}



/*---------------------------------------------------------------------------
 * Handle disconnected interface
 ---------------------------------------------------------------------------*/

int EInterfaces::interface_disconnected(int nEthernetNumber)
{
	debug("interface en%d disconnected\n", nEthernetNumber);

	if ( nEthernetNumber>=MAX_SUPPORTED_ETHERNET_CONNETIONS )
	{
		debugError("Invalid ethernet port\n");
		return -1;
	}
	
	m_aInterfaces[nEthernetNumber].m_ifnet = 0;
	m_aInterfaces[nEthernetNumber].m_fEnabled = FALSE;
	--m_nInterfacesInUse;

	update_interface_property();
	return 0;
}





/*---------------------------------------------------------------------------
 * Update all the registry properties associated with an interface
 ---------------------------------------------------------------------------*/
void EInterfaces::update_interface_property(void)
{
	int n;
	
	debug("update_interface_property - m_nInterfacesInUse=%d\n", m_nInterfacesInUse);

	if ( m_pProvider )
	{
		m_pProvider->removeProperty(ENABLED_INTERFACES_PROPERTY);
		
		if ( m_nInterfacesInUse )
		{
			OSArray* pInterfaces = OSArray::withCapacity(m_nInterfacesInUse);
			if ( pInterfaces )
			{
				for(n=0; n<numberof(m_aInterfaces); n++)
					if ( m_aInterfaces[n].m_fEnabled )
					{
						OSNumber* pNumber = OSNumber::withNumber(n, 32);
						if ( pNumber )
						{
							pInterfaces->setObject(pNumber);
							pNumber->release();
						}
					}
				
				m_pProvider->setProperty(ENABLED_INTERFACES_PROPERTY, (OSObject* )pInterfaces);
				pInterfaces->release();
			}
		}
	}
}


void EInterfaces::interface_reconnected(int nEthernetNumber, ifnet_t enetifnet)
{
	debug("interface en%d reconnected\n", nEthernetNumber);

	m_aInterfaces[nEthernetNumber].m_ifnet = enetifnet;
	m_aInterfaces[nEthernetNumber].m_fEnabled = TRUE;
	++m_nInterfacesInUse;
	
	recalculate_mtu();
	
	update_interface_property();
}



