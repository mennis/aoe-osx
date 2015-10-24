/*
 *  AoEDriverInterface.cpp
 *  AoEd
 *
 *  This class is used to communicate directly with the AoE kext.
 *  It can only be called with root permissions
 * 
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <sys/sys_domain.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "AoEDriverInterface.h"
#include "AoEcommon.h"
#include "AoEInterfaceCommands.h"
#include "debug.h"

// For a description of communicating with NKE kexts, see:
// http://developer.apple.com/documentation/Darwin/Conceptual/NKEConceptual/control/chapter_4_section_2.html#//apple_ref/doc/uid/TP40001858-CH227-CHDCHEHG

AoEDriverInterface::AoEDriverInterface()
{
	m_Socket = -1;
}

// Close the interface to our Driver. This should be called on exit
AoEDriverInterface::~AoEDriverInterface()
{
	disconnect();
}

int AoEDriverInterface::connect_to_driver()
{
	int ret;
    struct ctl_info ctl_info;
	struct sockaddr_ctl sc;

	m_Socket = -1;

	if ( 0 != geteuid() )
	{
		debugError("Cannot communicate with the kext without root privelages\n");
		return -1;
	}
	
	// Open our socket for communication
    m_Socket = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
	if (m_Socket < 0) 
	{
		debugError("socket SYSPROTO_CONTROL\n");
	}

	// Find our dynamically allocated ID based on our name
	bzero(&ctl_info, sizeof(struct ctl_info));
	strncpy(ctl_info.ctl_name, AOE_KEXT_NAME_Q, sizeof(struct ctl_info));

	if ( ioctl(m_Socket, CTLIOCGINFO, &ctl_info) == -1 )
	{
		debugVerbose("aoe driver isn't running\n");
		return -1;
	}

	bzero(&sc, sizeof(struct sockaddr_ctl));
	sc.sc_len = sizeof(struct sockaddr_ctl);
	sc.sc_family = AF_SYSTEM;
	sc.ss_sysaddr = SYSPROTO_CONTROL;
	sc.sc_id = ctl_info.ctl_id;
	sc.sc_unit = 0;
	
	// Now connect to our kext (this will fail without root privelages)
	ret = connect(m_Socket, (struct sockaddr *)&sc, sizeof(struct sockaddr_ctl));
	if ( ret )
	{
		debugError("Trouble connecting to our kext: Error = %d\n", errno);
		close(m_Socket);
		return ret;
	}

	//debugVerbose("Connected to driver\n");
	return 0;
}

int AoEDriverInterface::disconnect(void)
{
	// Close the socket, invalidate and leave...
	if ( -1!=m_Socket )
		close(m_Socket);
	m_Socket = -1;

	return 0;
}

#pragma mark -
#pragma mark General command passing

int AoEDriverInterface::set_command(int nCommand, void* pData, socklen_t Size)
{
	int ret = -1;

	if ( -1!=m_Socket )
	{
		ret = setsockopt(m_Socket, SYSPROTO_CONTROL, nCommand, pData, Size);
		if ( -1==ret )
		{
			debugError("Trouble sending command %d using setsockopt (error=%d)\n", nCommand, errno);
			return -1;
		}
		ret = 0;
	}
	return ret;
}

int AoEDriverInterface::get_command(int nCommand, void* pData, socklen_t Size)
{
	int ret = -1;
	socklen_t ReadSize = Size;
	
	if ( -1!=m_Socket )
	{
		if ( getsockopt(m_Socket, SYSPROTO_CONTROL, nCommand, pData, &ReadSize) == -1 )
		{
			debugError("Trouble with get_command %d using getsockopt (err=%d)\n", nCommand, errno);
			return -1;
		}
		if ( ReadSize!=Size )
		{
			debugError("get_command, size mismatch (received %d, but expected %d)\n", ReadSize, Size);
			return -1;
		}
		ret = 0;
	}
	return ret;
}

#pragma mark -
#pragma mark set_commands

int AoEDriverInterface::set_preference_settings(AoEPreferencesStruct* pPrefs)
{
	return set_command(AOEINTERFACE_PREFERENCES, pPrefs, sizeof(AoEPreferencesStruct));
}

int AoEDriverInterface::enable_logging(int* pnEnableLogging)
{
	return set_command(AOEINTERFACE_VERBOSE_LOGGING, pnEnableLogging, sizeof(int));
}


int AoEDriverInterface::force_packet_send(ForcePacketInfo* pPacketInfo)
{
	return set_command(AOEINTERFACE_FORCE_PACKET, pPacketInfo, sizeof(ForcePacketInfo));
}

int AoEDriverInterface::set_config_string(ConfigString* pCStringInfo)
{
	return set_command(AOEINTERFACE_SET_CONFIG_STRING, pCStringInfo, sizeof(ConfigString));
}

#pragma mark -
#pragma mark get_commands

int AoEDriverInterface::get_preference_settings(AoEPreferencesStruct* pPrefs)
{
	return get_command(AOEINTERFACE_PREFERENCES, pPrefs, sizeof(AoEPreferencesStruct));
}

int AoEDriverInterface::count_targets(int* pnTargets)
{
	return get_command(AOEINTERFACE_COUNT_TARGETS, pnTargets, sizeof(int));
}

int AoEDriverInterface::get_target_info(int nTarget, TargetInfo* pTargetInfo)
{
	pTargetInfo->nTargetNumber = nTarget;

	return get_command(AOEINTERFACE_GET_TARGET_INFO, pTargetInfo, sizeof(TargetInfo));
}

int AoEDriverInterface::get_error_info(ErrorInfo* pErrInfo)
{
	return get_command(AOEINTERFACE_GET_ERROR_INFO, pErrInfo, sizeof(ErrorInfo));
}

int AoEDriverInterface::get_payload_size(UInt32* pPayload)
{
	return get_command(AOEINTERFACE_GET_PAYLOAD_SIZE, pPayload, sizeof(UInt32));
}




