/*
 *  AoEInterfaceCommands.h
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#ifndef __AOE_INTERFACE_COMMANDS_H__
#define __AOE_INTERFACE_COMMANDS_H__

#include "AoEcommon.h"

enum AoEInterfaceCommands
{
	// Set all preference data in the kext (passes: AoEPreferencesStruct)
	AOEINTERFACE_PREFERENCES = 1,

	// Enable/Disable logging (passes: int)
	AOEINTERFACE_VERBOSE_LOGGING,

	// Force packet to pass through interface (passes: ForcePacketInfo)
	AOEINTERFACE_FORCE_PACKET,
	
	// Force an update of AoE targets and return the number of targets found. (Returns: int)
	AOEINTERFACE_COUNT_TARGETS,

	// Gets info about a particular target (returns: TargetInfo)
	AOEINTERFACE_GET_TARGET_INFO,
	
	// Gets info about error
	AOEINTERFACE_GET_ERROR_INFO,
	
	// Report the payload size per packet
	AOEINTERFACE_GET_PAYLOAD_SIZE,
	
	// Set the config string
	AOEINTERFACE_SET_CONFIG_STRING
};

#endif //__AOE_INTERFACE_COMMANDS_H__
