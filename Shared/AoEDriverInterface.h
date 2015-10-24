/*
 *  AoEDriverInterface.h
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_DRIVER_INTERFACE__
#define __AOE_DRIVER_INTERFACE__

#include <sys/socket.h>
#include <libkern/OSTypes.h>
#include "AoEInterfaceCommands.h"

class AoEDriverInterface
{
public:
	AoEDriverInterface();
	virtual ~AoEDriverInterface();
	int connect_to_driver();
	int disconnect(void);

	int set_preference_settings(AoEPreferencesStruct* pPrefs);
	int get_preference_settings(AoEPreferencesStruct* pPrefs);
	
	int count_targets(int* pnTargets);
	int get_target_info(int nTarget, TargetInfo* pTargetInfo);
	int get_error_info(ErrorInfo* pErrInfo);
	int get_payload_size(UInt32* pPayload);
	int set_config_string(ConfigString* pCStringInfo);

	int enable_logging(int* pnEnableLogging);
	int force_packet_send(ForcePacketInfo* pPacketInfo);
private:
	int set_command(int nCommand, void* pData, socklen_t Size);
	int get_command(int nCommand, void* pData, socklen_t Size);

	int		m_Socket;
};

#endif		//__AOE_DRIVER_INTERFACE__
