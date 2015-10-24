/*
 *  AoEtherFilter.c
 *  AoEInterface
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#include <sys/lock.h>
#include <sys/socket.h>
#include <sys/kpi_mbuf.h>
#include <net/kpi_interfacefilter.h>
#include <net/if.h>
#include <sys/kernel.h>
#include <sys/kern_event.h>
#include "AoEService.h"
#include "../Shared/AoEcommon.h"
#include "AoEtherFilter.h"
#include "aoe.h"
#include "debug.h"

#ifdef DEBUGBUILD
int				g_nVerboseLogging = 1;
#else
int				g_nVerboseLogging = 0;
#endif

static void free_locks(void);
static errno_t alloc_locks(void);

// There is only one copy of the C++ class and we keep a reference of it here when it is created.
// The C interface class will use this during communications
static void* g_pController = NULL;

void set_filtering_controller(void* pController)
{
	g_pController = pController;
}

typedef struct _InterfaceInfo
{
	interface_filter_t	Interface_Filters;
	ifnet_t				ifnet;
} InterfaceInfo;

InterfaceInfo	g_aInterfaces[MAX_SUPPORTED_ETHERNET_CONNETIONS];

//#define DEBUG_ALL_PACKETS_ON_INTERFACE

// set this define to 0 for for simple filtering of data.
#define SHOWDEBUGMESSAGES	1	// set to 1 to show debug messages, else set to 0 to disable messages

#define kMY_TAG_TYPE		1

// values to use with the memory allocated by the tag function
enum
{
	INBOUND_DONE	= 1,
	OUTBOUND_DONE
};

// 
#define NUM_HEADER_BYTES_TO_PRINT	30	// defines the number of bytes in the packet header to print

/* tag associated with this kext for use in marking packets that have been previously processed. 
 Note that even if you don't swallow/re-inject packets, it's a good idea to mark them, unless
 you don't care whether you will see the same packet again. Another interface filter could
 swallow/re-inject the packet and your filter will be called to process the packet again.
 */
static mbuf_tag_id_t	gidtag;

/* =================================== */
#pragma mark Utility Functions

/*
 CheckTag - see if there is a tag associated with the mbuf_t with the matching bitmap bits set in the
 memory associated with the tag. Use global gidtag as id Tag to look for
 input m - mbuf_t variable on which to search for tag
 value - value to compare in the tag_ref field associated with the tag
 return 1 - success, the bitmap image set in allocated memory associated with tag gidtag has the same bits set
 as does the bitmap
 return 0 - failure, either the mbuf_t is not tagged, or the allocated memory does not have the 
 expected value	
 */
static int	CheckTag(mbuf_t m, int value)
{
	errno_t	status;
	int* tag_ref;
	size_t	len;
	
	// Check whether we have seen this packet before.
	status = mbuf_tag_find(m, gidtag, kMY_TAG_TYPE, &len, (void**)&tag_ref);

	if ((status == 0) && (*tag_ref & value) && (len == sizeof(int))) 
		return 1;
	
	return 0;
}

/*
 SetTag - Set the tag associated with the mbuf_t with the bitmap bits set in bitmap
 input m - mbuf_t variable on which to search for tag
 bitmap - bitmap field to set in allocated memory
 return 0 - success, the tag has been allocated and for the mbuf_t and the bitmap bits has been set in the
 allocated memory. 
 anything else - failure
 
 */
static errno_t	SetTag(mbuf_t m, int value)
{	
	errno_t status;
	int* tag_ref;
	size_t len;
	
	// look for existing tag
	status = mbuf_tag_find(m, gidtag, kMY_TAG_TYPE, &len, (void*)&tag_ref);
	// allocate tag if needed
	if (status != 0)
	{
		// note that setting the MBUF_DONTWAIT flag for mbuf_tag memory allocation while within a packet
		// processing call will not deadlock packet processing under OS X 10.4 and greater.
		status = mbuf_tag_allocate(m, gidtag, kMY_TAG_TYPE, sizeof(int), MBUF_DONTWAIT, (void**)&tag_ref);
		if (status == 0)
			*tag_ref = 0;		// init tag_ref
		else
			debugError("mbuf_tag_allocate failed - result was %d\n", status);
	}
	else
	{
		// the tag exists - verify that the length of the tag_ref is the expected size
		
		// this should not happen
		if (len != sizeof(int))
		{
			debugError("tag detected at incorrect length - %d\n", len);
			status = EINVAL;	// invalid argument detected.
		}
	}

	if (status == 0) 
		*tag_ref = value;
	return status;
}

/*
 PrintPacketHeader - prints the first N bytes of the Ethernet packet as specified by
 NUM_HEADER_BYTES_TO_PRINT
 input: data - pointer to  mbuf_t of the packet
 */
static void	PrintPacketHeader(mbuf_t *data)
{
	int			bytesLeftToPrint;
	int			i, j, bytesToPrintNow;
	mbuf_t		m;
	char*		frame;
	
	m = *data;
	bytesLeftToPrint = NUM_HEADER_BYTES_TO_PRINT;
	j = 0;
	do
	{
		bytesToPrintNow = mbuf_len(m);	// count the number of bytes in the mbuf
		if (bytesToPrintNow > bytesLeftToPrint)	// limit the number of bytes to print
		{
			bytesToPrintNow = bytesLeftToPrint;
		}
		bytesLeftToPrint -= bytesToPrintNow;	// decrement bytesLeftToPrint to what remains 
		// to be printed
		
		frame = mbuf_data(m);
		for (i = 0; i < bytesToPrintNow; i++, j++)
		{
			debugShort("%02X", (u_int8_t)frame[i]);
			if (j == 5) debugShort("  "); // print space after the destination address
			if (j == 11) debugShort("  "); // print space after the source address
			if (j == 13) debugShort("  "); // print space after the protocol/length field
		}
		m = mbuf_next(m);
	} while ((m != NULL) && (bytesLeftToPrint != 0));
	
}

/* =================================== */
#pragma mark Filter Functions
/*!
 @typedef aoefilter_input_func
 
 @discussion iff_input_func is used to filter incoming packets. The
 interface is only valid for the duration of the filter call. If
 you need to keep a reference to the interface, be sure to call
 ifnet_reference and ifnet_release.
 @param cookie The cookie specified when this filter was attached.
 @param interface The interface the packet was recieved on.
 @param protocol The protocol of this packet. If you specified a
 protocol when attaching your filter, the protocol will only ever
 be the protocol you specified. By passing the protocol to your
 filter, you can supply the same function pointer to filters
 registered for different protocols and differetiate the protocol
 using this parameter.
 @param data The inbound packet, may be changed.
 @param frame_ptr A pointer to the pointer to the frame header.
 @result Return:
 0 - The caller will continue with normal processing of the packet.
 EJUSTRETURN - The caller will stop processing the packet, the packet will not be freed.
 Anything Else - The caller will free the packet and stop processing.
 */
static errno_t aoefilter_input_func(void* cookie, ifnet_t interface, protocol_family_t protocol, mbuf_t *data, char **frame_ptr)
{
	errno_t err;
	struct ether_header* pEHeader;

	//debug("ININININININININININININININININININININININININININININININININININININININININ\n");
	
	pEHeader = (struct ether_header*)*frame_ptr;

	// check whether we have seen this packet previously
	if ( CheckTag(*data, INBOUND_DONE) )
	{
		/* we have processed this packet previously since the INBOUND_DONE bit was set.
		 bail on further processing */
		debugWarn("Bailing on processing of this packet as we've seen it before...\n");
		return 0;
	}

	/*
	 If another nke swallows and re-injects the packet we will see the packet an
	 additional time. Rather than cache the mbuf_t reference we tag the mbuf_t and
	 search for it's presence which we have already done above to decide if we have processed the packet.
	 */
	err = SetTag(*data, INBOUND_DONE);
	
	if (err != 0)
	{
		//  mbuf_tag_allocate failed.
		debugError("Error - mbuf_tag_allocate returned an error %d\n", err);
		return err;	// stops processing and will cause the mbuf_t to be released
	}	

	if ( g_nVerboseLogging )
	{
#ifdef DEBUG_ALL_PACKETS_ON_INTERFACE
		debugShort("aoefilter_input_func     - ");

		switch ( protocol )
		{
			case AF_UNSPEC:     debugShort("UNSPEC    - "); break;
			case AF_INET:		debugShort("TCP/IP    - "); break;
			case AF_APPLETALK:	debugShort("AppleTalk - "); break;
			default:			debugShort("proto  %d - ", protocol); break;
		}

		PrintPacketHeader(data);

		{
			u_int32_t	bytes = 0;
			mbuf_t	mCopy = *data;
			do
			{
				bytes += mbuf_len(mCopy);
				mCopy = mbuf_next(mCopy);
			} while (mCopy != NULL);

			debugShort("  %d bytes incoming\n", bytes);
		}
#endif		
	}

#if 0
	{
		int n;
		FireLog("A: DEST- ");
		// TODO: Swap these to host order
		for(n=0; n<ETHER_ADDR_LEN; n++)
			FireLog("%02x:", pEHeader->ether_dhost[n]);
		FireLog("-SRC-");
		for(n=0; n<ETHER_ADDR_LEN; n++)
			FireLog("%02x:", pEHeader->ether_shost[n]);
		FireLog("\n");
	}
#endif

	if ( ifnet_hdrlen(interface) != sizeof(struct ether_header) )
		debugError("Unexpected frame header size on interface");

	// Check for our ethernet type and then process the incoming packet
	if ( (AF_UNSPEC==protocol) && pEHeader->ether_type==htons(ETHERTYPE_AOE) )
		if ( 0 != c_aoe_incoming(g_pController, interface, pEHeader, data) )
			debugError("Trouble processing incoming AoE packet\n");
	
	//debug("OUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUTOUT\n");

	return 0;
}

/*!
 @typedef aoefilter_output_func
 
 @discussion iff_output_func is used to filter fully formed outbound
 packets. This function is called after the protocol specific
 preoutput function. The interface is only valid for the duration
 of the filter call. If you need to keep a reference to the
 interface, be sure to call ifnet_reference and ifnet_release.
 @param cookie The cookie specified when this filter was attached.
 @param interface The interface the packet is being transmitted on.
 @param data The outbound packet, may be changed.
 @result Return:
 0 - The caller will continue with normal processing of the packet.
 EJUSTRETURN - The caller will stop processing the packet, the packet will not be freed.
 Anything Else - The caller will free the packet and stop processing.
 */
static errno_t aoefilter_output_func(void* cookie, ifnet_t interface, protocol_family_t protocol, mbuf_t *data)
{
	errno_t			err;
#ifdef DEBUG_ALL_PACKETS_ON_INTERFACE
	mbuf_t			m = *data;
#endif
	
	/* check whether we have seen this packet previously */
	if (CheckTag(*data, OUTBOUND_DONE))
	{
		/* we have processed this packet previously since the OUTBOUND_DONE bit was set.
		 bail on further processing
		 */
		return 0;
	}
	
#ifdef DEBUG_ALL_PACKETS_ON_INTERFACE
	/*
	 If we reach this point, then we have not previously seen this packet. 
	 First lets get some statistics from the packet.
	 */
	if ( g_nVerboseLogging )
	{
		debugShort("aoefilter_output_func    - ");
		switch (protocol)
		{
			case AF_UNSPEC:     debugShort("UNSPEC    - "); break;
			case AF_INET:		debugShort("TCP/IP    - "); break;
			case AF_APPLETALK:	debugShort("AppleTalk - "); break;
			default:			debugShort("proto  %d - ", protocol); break;
		}
	}
	
	// count the number of outgoing bytes
	byteInPacket = 0;
	do
	{
		byteInPacket += mbuf_len(m);
		m = mbuf_next(m);
	} while (m != NULL);
	
	if ( g_nVerboseLogging )
	{
		PrintPacketHeader(data);
		debugShort("  %d bytes outgoing\n", byteInPacket);
	}
#endif
	
	/*
	 If we swallow the packet and later re-inject the packet, we have to be
	 prepared to see the packet through this routine once again. In fact, if
	 after re-injecting the packet, another nke swallows and re-injects the packet
	 we will see the packet an additional time. Rather than cache the mbuf_t reference
	 we tag the mbuf_t and search for it's presence which we have already done above
	 to decide if we have processed the packet.
	 */
	err = SetTag(*data, OUTBOUND_DONE);
	
	if (err != 0)
	{
		//  mbuf_tag_allocate failed.
		debugError("Error - mbuf_tag_allocate returned an error %d\n", err);
		return err;	// stops processing and will cause the mbuf_t to be released
	}
	return 0;
}

/*!
 @typedef aoefilter_event_func
 
 @discussion iff_event_func is used to filter interface specific
 events. The interface is only valid for the duration of the
 filter call. If you need to keep a reference to the interface,
 be sure to call ifnet_reference and ifnet_release.
 @param cookie The cookie specified when this filter was attached.
 @param interface The interface the packet is being transmitted on.
 @param event_msg The kernel event, may not be changed.
 */
static void aoefilter_event_func(void* cookie, ifnet_t interface, protocol_family_t protocol,
				const struct kev_msg *event_msg)
{
	int nInterfaceNumber;

	// Find our interface number
	for (nInterfaceNumber=0; nInterfaceNumber<numberof(g_aInterfaces); nInterfaceNumber++)
		if ( interface==g_aInterfaces[nInterfaceNumber].ifnet )
			break;

	switch ( event_msg->event_code )
	{
		case KEV_DL_LINK_OFF :
		{
			if ( g_nVerboseLogging )
				debug("Interface gone!!\n");

			c_interface_disconnected(g_pController, nInterfaceNumber);
			break;
		}
		case KEV_DL_LINK_ON :
		{
			if ( g_nVerboseLogging )
				debug("Interface back!!\n");

			c_interface_reconnected(g_pController, nInterfaceNumber, interface);
			break;
		}
		case KEV_DL_SIFFLAGS :
		case KEV_DL_SIFMETRICS :
		case KEV_DL_SIFMTU :
		case KEV_DL_SIFPHYS :
		case KEV_DL_SIFMEDIA :
		case KEV_DL_SIFGENERIC :
		case KEV_DL_ADDMULTI :
		case KEV_DL_DELMULTI :
		case KEV_DL_IF_ATTACHED :
		case KEV_DL_IF_DETACHING :
		case KEV_DL_IF_DETACHED :
		case KEV_DL_PROTO_ATTACHED :
		case KEV_DL_PROTO_DETACHED :
		case KEV_DL_LINK_ADDRESS_CHANGED :
		default :
		{
			if ( g_nVerboseLogging )
			{
				debug("aoefilter_event_func     -  vendor %ld, class %ld, subclass %ld, event code %ld\n",
					  event_msg->vendor_code, event_msg->kev_class, 
					  event_msg->kev_subclass, event_msg->event_code);

				debugWarn("Interface event not handled!!\n");
			}
		}
	}

	return;
}

/*!
 @typedef aoefilter_ioctl_func
 
 @discussion iff_ioctl_func is used to filter ioctls sent to an
 interface. The interface is only valid for the duration of the
 filter call. If you need to keep a reference to the interface,
 be sure to call ifnet_reference and ifnet_release.
 @param cookie The cookie specified when this filter was attached.
 @param interface The interface the packet is being transmitted on.
 @param ioctl_cmd The ioctl command.
 @param ioctl_arg A pointer to the ioctl argument.
 @result Return:
 (NOTE: The following return result description is a correction to the return results presented 
 in kpi_interfacefilter.h provided with 10.4.
 EOPNOTSUPP(or ENOTSUP) - indicates that the ioctl was not processed - the caller will 
 continue with normal processing of the packet.
 EJUSTRETURN - indicates that no further processing of the ioctl is desired - the caller 
 will stop processing the packet, the packet will not be freed.
 0 - indicates that the ioctl was handled, however, the caller will continue processing 
 the packet with other filter functions.
 Anything Else - The caller will free the packet and stop processing.
 */
static errno_t aoefilter_ioctl_func(void* cookie, ifnet_t interface, protocol_family_t protocol, u_long ioctl_cmd, void* ioctl_arg)
{
	debugShort("aoefilter_ioctl_func     - ");
	switch (protocol)
	{
		case AF_INET:		debugShort("TCP/IP,"); break;
		case AF_APPLETALK:	debugShort("AppleTalk,"); break;
		default:			debugShort("Unknown protocol: %d,", protocol); break;
	}
	debugShort(" cmd is 0x%X\n", ioctl_cmd);
	return EOPNOTSUPP;
}

/*!
 @typedef aoefilter_detached_func
 
 @discussion iff_detached_func is called to notify the filter that it
 has been detached from an interface. This is the last call to
 the filter that will be made. A filter may be detached if the
 interface is detached or the detach filter function is called.
 In the case that the interface is being detached, your filter's
 event function will be called with the interface detaching event
 before the your detached function will be called.
 @param cookie The cookie specified when this filter was attached.
 @param interface The interface this filter was detached from.
 */
static void aoefilter_detached_func(void* cookie, ifnet_t interface)
{
	debug("aoefilter_detached_func entered\n");
	return;
}

// This structure defines the callback functions we'll use for filtering
static struct iff_filter s_Enet_filter =
{
	NULL,				/* iff_cookie field */
	AOE_KEXT_NAME_Q,
	0,					/* interested in all protocol packets */
	aoefilter_input_func,
	aoefilter_output_func,
	aoefilter_event_func,
	aoefilter_ioctl_func,
	aoefilter_detached_func
};



/* =================================== */
#pragma mark --
#pragma mark Initialisation

kern_return_t filter_init(void)
{
	kern_return_t retval;
	int n;
	
	for(n=0; n<numberof(g_aInterfaces); n++)
		g_aInterfaces[n].Interface_Filters = 0;
	
	// set up the tag value associated with this NKE in preparation for swallowing packets and re-injecting them
	retval = mbuf_tag_id_find(AOE_KEXT_NAME_Q , &gidtag);
	if (retval != 0)
		debug("mbuf_tag_id_find returned error %d\n", retval);

	alloc_locks();

	return retval;
}

void filter_uninit(void)
{
	int n;
	
	// Disable all filtering	
	for(n=0; n<numberof(g_aInterfaces); n++)
		if ( g_aInterfaces[n].Interface_Filters )
			disable_filtering(n);
	
	free_locks();
}

#pragma mark --
#pragma mark Preference handling

int getVerboseAoELogging(void)
{
	return g_nVerboseLogging;
}

void enable_filter_logging(int nEnableLogging)
{
	g_nVerboseLogging = nEnableLogging;
}


#pragma mark --
#pragma mark Lock handling

static errno_t alloc_locks(void)
{
	return 0;
}

/* 
 free_locks - used to free the fine grain locks for use in controlling access to the SwallowPktQueue structures. 
*/

static void free_locks(void)
{
}

#pragma mark --
#pragma mark Setup


kern_return_t enable_filtering(int nEthernetNumber, ifnet_t Enetifnet)
{
	kern_return_t retval;
	
	debug("enable_filtering\n");
	
	retval = iflt_attach(Enetifnet, &s_Enet_filter, &g_aInterfaces[nEthernetNumber].Interface_Filters);

	if ( 0==g_aInterfaces[nEthernetNumber].Interface_Filters )
		debugError("CODE ASSUMES interface_filter_t != 0");
	
	g_aInterfaces[nEthernetNumber].ifnet = Enetifnet;
	
	if (retval == KERN_SUCCESS)
		;
	else
		goto error;
	
    return retval;
	
error:
	debugError("Trouble Enabling AoE filtering on interface: en%d", nEthernetNumber);
	return KERN_FAILURE;
}

kern_return_t disable_filtering(int nEthernetNumber)
{
	kern_return_t		retval;
	
	retval = KERN_FAILURE; // default result, unless we know that we are 

	debug("disable_filtering\n");
	debug("getting lock...\n");

	// detached from the interface.
	if ( g_aInterfaces[nEthernetNumber].Interface_Filters )
	{
		debug("performing detach...\n");
		iflt_detach(g_aInterfaces[nEthernetNumber].Interface_Filters);
		debug("detach complete...\n");
		retval == KERN_SUCCESS;
	}
	else
		debug("Nothing to disable for this interface");
	
    return retval;
}
