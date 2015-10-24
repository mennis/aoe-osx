/*
 *  AoEUserInterface.c
 *  AoEInterface
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <IOKit/IOLib.h>
#include <sys/errno.h>
#include <sys/kernel_types.h>
#include <sys/kern_control.h>
#include "AoEService.h"
#include "AoEInterfaceCommands.h"
#include "AoEUserInterface.h"
#include "debug.h"
#include "../Shared/AoEcommon.h"


// This should be large enough to pass the largest data block across the user/kernel interface
#define INTERFACE_BUFFER		(8*1024)


// There is only one copy of the C++ class and we keep a reference of it here when it is created.
// The C interface class will use this during communications
static void* g_pController = NULL;
static kern_ctl_ref		g_CtrlRef = NULL;
static AoEPreferencesStruct g_PreferenceData;

// Fine grain locking variables
static lck_mtx_t*	g_mutex = NULL;
static lck_grp_t*	g_mutex_grp = NULL;

static errno_t alloc_locks(void);
static void free_locks(void);

// This is also used to init/unint the c globals
void set_ui_controller(void* pController)
{
	g_pController = pController;
	
	if ( g_pController )
		alloc_locks();
	else
		free_locks();
}

#pragma mark -
#pragma mark Control Socket handling (User interface)

/*
 * We have a controlsocket expressing interest. 
 */

static int aoeinterface_connect(kern_ctl_ref ctl_ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	errno_t error = 0;
	
	debugVerbose("Opening AoE communications^^^^^^^^^^^^^^^\n");

	if ( g_mutex )
		lck_mtx_lock(g_mutex);
	
	return error;
}

/*!
 @typedef aoeinterface_disconnect_func
 @discussion The aoeinterface_disconnect_func is used to receive notification
 that a client has disconnected from the kernel control. This
 usually happens when the socket is closed. If this is the last
 socket attached to your kernel control, you may unregister your
 kernel control from this callback.
 @param kctlref The control ref for the kernel control instance the client has
 disconnected from.
 @param unit The unit number of the kernel control instance the client has
 disconnected from.  
 @param unitinfo The unitinfo value specified by the connect function
 when the client connected.
 */

static errno_t aoeinterface_disconnect(kern_ctl_ref ctl_ref, u_int32_t unit, void *unitinfo)
{
	debugVerbose("Closing AoE communications^^^^^^^^^^^^^^^\n");

	if ( g_mutex )
		lck_mtx_unlock(g_mutex);

	return 0;
}


/*!
 @typedef aoeinterface_get
 @discussion The aoeinterface_get is used to handle client get socket
 option requests for the SYSPROTO_CONTROL option level. A buffer
 is allocated for storage and passed to the function. The length
 of that buffer is also passed. Upon return, you should set *len
 to length of the buffer used. In some cases, data may be NULL.
 When this happens, *len should be set to the length you would
 have returned had data not been NULL. If the buffer is too small,
 return an error.
 @param kctlref The control ref of the kernel control.
 @param unit The unit number of the kernel control instance.
 @param unitinfo The unitinfo value specified by the connect function
 when the client connected.
 @param opt The socket option.
 @param data A buffer to copy the results in to. May be NULL, see
 discussion.
 @param len A pointer to the length of the buffer. This should be set
 to the length of the buffer used before returning.
 */

static int aoeinterface_get(kern_ctl_ref ctl_ref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t *len)
{
	UInt32		unData;
	int			nData;
	TargetInfo	Target;
	ErrorInfo	EInfo;
	int			error = 0;
	size_t		valsize;
	void*		pBuf = 0;

	debug("aoeinterface_get - opt is %d | data is %p\n", opt, data);

	switch ( opt )
	{
		case AOEINTERFACE_PREFERENCES:
		{
			if ( sizeof(AoEPreferencesStruct) != *len )
				debugError("Unexpected size\n");

			valsize = min(sizeof(AoEPreferencesStruct), *len);
			pBuf = &g_PreferenceData;
			break;
		}
		case AOEINTERFACE_VERBOSE_LOGGING :
		{
			if ( sizeof(int) != *len )
				debugError("Unexpected size\n");
			
			valsize = min(sizeof(int), *len);
			nData = c_get_logging(g_pController);
			pBuf = &nData;
			break;
		}
		case AOEINTERFACE_COUNT_TARGETS :
		{
			if ( sizeof(int) != *len )
				debugError("Unexpected size\n");

			valsize = min(sizeof(int), *len);
			nData = 0;
			c_update_target(g_pController, &nData);
			debug("AOEINTERFACE_COUNT_TARGETS - found %d\n", nData);
			pBuf = &nData;
			break;
		}
		case AOEINTERFACE_GET_TARGET_INFO :
		{
			if ( sizeof(TargetInfo) != *len )
				debugError("Unexpected size\n");
	
			valsize = min(sizeof(TargetInfo), *len);

			debug("Getting target info\n");
			c_update_target(g_pController, NULL);

			TargetInfo* TargetIncoming = (TargetInfo*) data;
			debug("data=%p target=%p valsize=%d\n", data, TargetIncoming, valsize);
			if ( data )
			{
				debug("Getting target info for target: %d\n", TargetIncoming->nTargetNumber);
				if ( 0!=c_get_target_info(g_pController, TargetIncoming->nTargetNumber, &Target) )
				{
					debugError("Unable to get target info\n");
					error = EIO;
					break;
				}
			}
			else
				debugError("Data pointer isn't initialised\n");

			// Copy this information across
			Target.nTargetNumber = TargetIncoming->nTargetNumber;
			pBuf = &Target;
			break;
		}
		case AOEINTERFACE_GET_ERROR_INFO :
		{
			if ( sizeof(ErrorInfo) != *len )
				debugError("Unexpected size\n");

			valsize = min(sizeof(ErrorInfo), *len);
			c_get_error_info(g_pController, &EInfo);
			
			//debug("unexpected=%d\n", EInfo.nUnexpectedResponses);
			//debug("nRetransmits=%d\n", EInfo.nRetransmits);

			pBuf = &EInfo;
			break;
		}
		case AOEINTERFACE_GET_PAYLOAD_SIZE :
		{
			if ( sizeof(UInt32) != *len )
				debugError("Unexpected size\n");
			
			valsize = min(sizeof(UInt32), *len);
			nData = 0;
			c_get_payload_size(g_pController, &unData);
			debug("AOEINTERFACE_GET_PAYLOAD_SIZE - found %d\n", unData);
			pBuf = &unData;
			break;
		}
		default:
			error = ENOTSUP;
			break;
	}

	if ( error == 0 )
	{
		if ( len != NULL )
			*len = valsize;
		else
			debugError("Invalid Length\n");
		
		if ( (data != NULL) && (pBuf != NULL) )
			bcopy(pBuf, data, valsize);
		else
			debugError("Invalid data pointer\n");
	}
	

	return error;
}

/*!
 @typedef aoeinterface_setopt_func
 @discussion The aoeinterface_setopt_func is used to handle set socket option
 calls for the SYSPROTO_CONTROL option level.
 @param kctlref The control ref of the kernel control.
 @param unit The unit number of the kernel control instance.
 @param unitinfo The unitinfo value specified by the connect function
 when the client connected.
 @param opt The socket option.
 @param data A pointer to the socket option data. The data has
 already been copied in to the kernel for you.
 @param len The length of the socket option data.
 */

static int aoeinterface_set(kern_ctl_ref ctl_ref, u_int32_t unit, void *unitinfo, int opt, void *pData, size_t len)
{
	int nError = 0;
	
	debug("aoeinterface_set - opt is %d\n", opt);
	
	if ( 0==pData )
		return EFAULT;
	
	switch ( opt )
	{
		case AOEINTERFACE_PREFERENCES:
		{
			int n, nNew, nPrev;

			if (len < sizeof(AoEPreferencesStruct))
			{
				debugError("AOEINTERFACE_PREFERENCES: Size of input is incorrect (was=%d)\n", len);
				nError = EINVAL;
				break;
			}

			AoEPreferencesStruct PreviousPrefs = g_PreferenceData;
			// Copy data to local variable
			g_PreferenceData = *((AoEPreferencesStruct*)pData);

			if ( g_PreferenceData.nNumberOfPorts> MAX_SUPPORTED_ETHERNET_CONNETIONS )
			{
				debugError("Invalid number of ports\n");
				g_PreferenceData.nNumberOfPorts = MAX_SUPPORTED_ETHERNET_CONNETIONS;
			}

			for(n=0; n<PreviousPrefs.nNumberOfPorts; n++)
				debug("Previous port[%d] = %d\n", n, PreviousPrefs.anEnabledPorts[n]);
	
			for(n=0; n<g_PreferenceData.nNumberOfPorts; n++)
				debug("Current port[%d] = %d\n", n, g_PreferenceData.anEnabledPorts[n]);

			// Enable any ports that have recently been added
			for(nNew=0; nNew<g_PreferenceData.nNumberOfPorts; nNew++)
			{
				bool fFound = FALSE;
				for(nPrev=0; nPrev<PreviousPrefs.nNumberOfPorts; nPrev++)
				{
					if ( PreviousPrefs.anEnabledPorts[nPrev]==g_PreferenceData.anEnabledPorts[nNew] )
						fFound = TRUE;
				}

				if ( !fFound )
					c_enable_interface(g_pController, g_PreferenceData.anEnabledPorts[nNew]);
			}
			
			// Disable any ports that have recently been removed
			for(nPrev=0; nPrev<PreviousPrefs.nNumberOfPorts; nPrev++)
			{
				bool fFound = FALSE;
				for(nNew=0; nNew<g_PreferenceData.nNumberOfPorts; nNew++)
					if ( PreviousPrefs.anEnabledPorts[nPrev]==g_PreferenceData.anEnabledPorts[nNew] )
						fFound = TRUE;
				
				if ( !fFound )
					c_disable_interface(g_pController, PreviousPrefs.anEnabledPorts[nPrev]);
			}	
			
			debug("config string=\"%s\"\n", g_PreferenceData.aszComputerConfigString);

			c_set_max_transfer_size(g_pController, g_PreferenceData.nMaxTransferSize);
			
			c_set_user_window(g_pController, g_PreferenceData.nUserBlockCountWindow);

			c_set_ourcstring(g_pController, (char*)g_PreferenceData.aszComputerConfigString);

			// Now that we've modified the interfaces, check for any change in the connected targets
			c_update_target(g_pController, NULL);
			break;
		}
		case AOEINTERFACE_VERBOSE_LOGGING :
		{
			if (len < sizeof(int))
			{
				debugError("AOEINTERFACE_VERBOSE_LOGGING: Size of input is incorrect (was=%d)\n", len);
				nError = EINVAL;
				break;
			}
			// Copy data to local variable
			int nEnableLogging = *((int*)pData);

			debug("EnableVerboseAoELogging = %d\n", nEnableLogging);
			c_set_logging(g_pController, nEnableLogging);
			break;
		}
		case AOEINTERFACE_FORCE_PACKET :
		{
			ForcePacketInfo* pForcedPacketInfo = (ForcePacketInfo*)pData;
			
			if ( len < sizeof(ForcePacketInfo) )
			{
				debugError("AOEINTERFACE_FORCE_PACKET: Size of input is incorrect (was=%d)\n", len);
				nError = EINVAL;
				break;
			}

			if ( 0!= c_force_packet(g_pController, pForcedPacketInfo) )
				debugError("Trouble sending forced packet\n");
			break;
		}
		case AOEINTERFACE_SET_CONFIG_STRING:
		{
			ConfigString* pConfigStringInfo = (ConfigString*)pData;

			if ( len < sizeof(ConfigString) )
			{
				debugError("AOEINTERFACE_SET_CONFIG_STRING: Size of input is incorrect (was=%d)\n", len);
				nError = EINVAL;
				break;
			}

			c_set_targets_cstring(g_pController, pConfigStringInfo);
			break;
		}
		default:
		{
			nError = ENOTSUP;
			break;
		}
	}

	return nError;
}


#pragma mark -
#pragma mark Lock handling

static errno_t alloc_locks(void)
{
	errno_t	result = 0;
	lck_grp_attr_t	*grp_attributes = NULL;
	lck_attr_t		*lck_attributes = NULL;
	
    /* Allocate a mutex lock group */
	/* allocate a lock group attribute var */
    grp_attributes = lck_grp_attr_alloc_init();
	if (grp_attributes)
	{
		/* set the default values for the lock group attribute var */
		lck_grp_attr_setdefault(grp_attributes);
		// for the name, use the reverse dns name associated with this
		// kernel extension
		/* allocate the lock group */
		g_mutex_grp = lck_grp_alloc_init(AOE_KEXT_NAME_Q, grp_attributes);
		if (g_mutex_grp == NULL)
		{
			debugError("Problem calling lck_grp_alloc_init\n");
			result = ENOMEM;
		}
		// can free the attributes once we've allocated the group lock
		lck_grp_attr_free(grp_attributes);
	}
	else
	{
		debugError("Problem calling lck_grp_attr_alloc_init\n");
		result = ENOMEM;
	}
	
	if (result == 0)
	{
		/* allocate a lock attribute var */
		lck_attributes = lck_attr_alloc_init();
		if (lck_attributes)
		{
			/* allocate the lock for use on processing items in the input queue */
			g_mutex = lck_mtx_alloc_init(g_mutex_grp, lck_attributes);
			if (g_mutex == NULL)
			{
				debugError("Problem calling lck_mtx_alloc_init\n");
				result = ENOMEM;
			}
			// can free the attributes once we've allocated the lock
			lck_attr_free(lck_attributes);
		}
		else
		{
			debugError("Problem calling lck_attr_alloc_init\n");
			result = ENOMEM;
		}
	}
	return result;	// if we make it here, return success
}

/* 
 free_locks - used to free the fine grain locks for use in controlling access to the SwallowPktQueue
 structures. 
 input - nothing
 output - nothing - since all of the kernel calls return void results.
 */

static void free_locks(void)
{
	if ( g_mutex )
	{
		lck_mtx_free(g_mutex, g_mutex_grp);
		g_mutex = NULL;
	}
	if ( g_mutex_grp )
	{
		lck_grp_free(g_mutex_grp);
		g_mutex_grp = NULL;
	}
}


#pragma mark -
#pragma mark System Control Structure Definition

// this is the new way to register a system control structure
// this is not a const structure since the ctl_id field will be set when the ctl_register call succeeds
static struct kern_ctl_reg gctl_reg =
{
	AOE_KEXT_NAME_Q,			// unique name
	0,							// set to 0 for dynamically assigned control ID - CTL_FLAG_REG_ID_UNIT not set
	0,							// ctl_unit - ignored when CTL_FLAG_REG_ID_UNIT not set
	CTL_FLAG_PRIVILEGED,		// privileged access required to access this filter
	INTERFACE_BUFFER,			// Override send buffer size
	INTERFACE_BUFFER,			// Override receive buffer size
	aoeinterface_connect,		// Called when a connection request is accepted
	aoeinterface_disconnect,	// called when a connection becomes disconnected
	NULL,						// called when data sent from the client to the kernel control.
	aoeinterface_set,			// called when the user process makes the setsockopt call
	aoeinterface_get			// called when the user process makes the getsockopt call
};


#pragma mark -
#pragma mark "Public" functions

int open_user_interface()
{
	int nReturn = -1;

	// register our control structure so that we can be found by a user level process.
	errno_t retval = ctl_register(&gctl_reg, &g_CtrlRef);
	if ( retval == 0 )
	{
		debugVerbose("ctl_register, ref %#x \n", g_CtrlRef);
		nReturn = 0;
	}
	else
	{
		debugError("ctl_register returned error %d\n", retval);
	}
	
	return nReturn;
}

int close_user_interface()
{
	if ( g_CtrlRef )
		ctl_deregister(g_CtrlRef);
	
	return 0;
}

