//
//  AoEcommon.h
//  AoEInterface
//
//  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
//

#ifndef __AOECOMMON_H__
#define __AOECOMMON_H__

#include <sys/kernel_types.h>
#include <sys/types.h>
#include "aoe.h"

__BEGIN_DECLS
#include <net/ethernet.h>
__END_DECLS

//------------------//
// Useful functions //
//------------------//

__BEGIN_DECLS
__private_extern__ uint64_t time_since_now_ms(uint64_t old_time);
__private_extern__ uint64_t time_since_now_us(uint64_t old_time);
__private_extern__ uint64_t time_since_now_ns(uint64_t old_time);
__END_DECLS

//---------------//
// Useful Macros //
//---------------//

#define numberof(x)				(sizeof(x)/sizeof(x[0]))
#define STR(x)					#x
#define CLEAN_RELEASE(thingPtr) if(thingPtr) {thingPtr->release(); thingPtr=NULL;}

#ifndef MAX
#define MAX(x,y)				((x)>(y)?(x):(y))
#endif
#ifndef MIN
#define MIN(x,y)				((x)<(y)?(x):(y))
#endif

#define CONVERT_NS_TO_US(x)		(((x)+500)/1000)		// This ensures the truncation is handled correctly
#define CONVERT_NS_TO_MS(x)		(((x)+500000)/1000000)	// This ensures the truncation is handled correctly

#define COUNT_SECTORS_FROM_MTU(MTU)			(((MTU)-BYTES_IN_AOE_HEADER)/kATADefaultSectorSize)

//--------------//
// Tag handling //
//--------------//

#define DEVICE_ONLINE_TAG						0
#define TAG_USER_MASK							0x80000000
#define TAG_BROADCAST_MASK						0x40000000

#define MIN_TAG									1
#define MAX_TAG									(TAG_BROADCAST_MASK-1)

//-----------//
// anomolies //
//-----------//

// These macros were added in 10.5, since we're building against 10.4u, we need to add it in ourselves
#ifndef TAILQ_FOREACH_SAFE
#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)															\
														for ((var) = TAILQ_FIRST((head));					\
														(var) && ((tvar) = TAILQ_NEXT((var), field), 1);	\
														(var) = (tvar))
#endif

#ifndef LIST_FOREACH_SAFE
#define	LIST_FOREACH_SAFE(var, head, field, tvar)															\
														for ((var) = LIST_FIRST((head));					\
														(var) && ((tvar) = LIST_NEXT((var), field), 1);		\
														(var) = (tvar))
#endif


//------------//
// KEXT names //
//------------//

// This constant is used throughout the code to search for out KEXT. The only place that doesn't use it is the com.aoed.plist file

#define AOE_KEXT_NAME						net_corvus_driver_aoe
#define AOE_KEXT_NAME_Q						"net_corvus_driver_aoe"

#define AOE_CONTROLLER_INTERFACE_NAME		net_corvus_aoe_controller_interface
#define AOE_CONTROLLER_INTERFACE_NAME_Q		"net_corvus_aoe_controller_interface"

#define AOE_CONTROLLER_NAME					net_corvus_aoe_controller
#define AOE_CONTROLLER_NAME_Q				"net_corvus_aoe_controller"

#define AOE_DEVICE_NAME						net_corvus_aoe_device
#define AOE_DEVICE_NAME_Q					"net_corvus_aoe_device"

//------------//
// properties //
//------------//

#define TARGET_PROPERTY						"Target"	// Unique number
#define SHELF_PROPERTY						"Shelf"
#define SLOT_PROPERTY						"Slot"
#define CAPACITY_PROPERTY					"Capacity"
#define TARGET_NUMER_PROPERTY				"Number"
#define CONFIG_STRING_PROPERTY				"Config String"
#define ATTACHED_INTERFACES_PROPERTY		"Interfaces"
#define BUFFER_COUNT_PROPERTY				"Buffer Count"

#define IDENT_CAPACITY_PROPERTY				"Identified Capacity"
#define IDENT_MODEL_PROPERTY				"Identified Model"
#define IDENT_SERIAL_PROPERTY				"Identified Serial"

#define ENABLED_INTERFACES_PROPERTY			"Enabled Interfaces"
#define OUR_CSTRING_PROPERTY				"Computer Config String"

//---------------//
// AoE constants //
//---------------//

// This is arbitrary and can be increased to provide support for more interfaces
#define	MAX_SUPPORTED_ETHERNET_CONNETIONS		6

// Just used for the forced packet commands on the user interface
#define AOEINTERFACE_MAX_PACKET_WORDS			10

#define DEFAULT_CONGESTION_WINDOW				128

//-------------------//
// Shared Structures //
//-------------------//

// NOTE:	Because these are shared between kernel/user space, the types should be of a fixed width.
//			...later, it could be an issue if the word widths changes between them

typedef struct _AoEPreferencesStruct
{
	uint32_t nNumberOfPorts;
	uint32_t nMaxTransferSize;
	uint32_t nUserBlockCountWindow;
	uint32_t anEnabledPorts[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	uint8_t aszComputerConfigString[MAX_CONFIG_STRING_LENGTH];
} AoEPreferencesStruct;

	
typedef struct _TargetInfo
{
	uint32_t	nTargetNumber;

	uint32_t	nSlot;
	uint32_t	nShelf;

	uint32_t	NumSectors;

	uint32_t	nNumberOfInterfaces;
	ifnet_t		aInterfaces[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	uint32_t	aInterfaceNum[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	u_char		aaSrcMACAddress[MAX_SUPPORTED_ETHERNET_CONNETIONS][ETHER_ADDR_LEN];
	u_char		aaDestMACAddress[MAX_SUPPORTED_ETHERNET_CONNETIONS][ETHER_ADDR_LEN];
	
	uint32_t	nLastSentInterface;
} TargetInfo;

typedef struct _ErrorInfo
{
	int		nUnexpectedResponses;
	int		nRetransmits;
} ErrorInfo;

	
typedef struct _ForcePacketInfo
{
	uint32_t	nShelf;
	uint32_t	nSlot;
	uint32_t	fATA;
	uint32_t	Tag;

	aoe_header	AoEhdr;
	aoe_atahdr	ATAhdr;
	aoe_cfghdr	CFGhdr;
} ForcePacketInfo;

typedef struct _ConfigString
{
	uint32_t	nTargetNumber;
	uint32_t	Length;

	char		pszConfig[MAX_CONFIG_STRING_LENGTH];
} ConfigString;

#endif		//__AOECOMMON_H__
