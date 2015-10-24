//
//  main.cpp
//  AoEd
//
//  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
//

#import <DiskArbitration/DiskArbitration.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include "AoEDriverInterface.h"
#include "AoEProperties.h"
#include "Preferences.h"
#include "debug.h"
#include "ConfigString.h"
#include "../Shared/EthernetDetect.h"


// This interface program is responsible for communicating with the AoE kext and handling preferences settings.
//
// It is used specifically in a number of ways:
//
// 1/ As a command line interface for controlling AoE functionality
// 2/ As a system level daemon controlling the recalling of preferences on startup
// 3/ As an interface to the AoE.kext used with the system preference pane.
// 

// For more info on launchd, see:
// LAUNCHD: http://developer.apple.com/documentation/MacOSX/Conceptual/BPSystemStartup/Articles/LaunchOnDemandDaemons.html#//apple_ref/doc/uid/TP40001762
// man launchd.plist
// To test this option on the command line, use: sudo launchctl load com.aoed.plist

typedef enum
{
	UnMountError = -1,
	UnMountOK = 0,
	UnMountIncomplete = 1,
} UnMountState;

// Set variable when unmount is complete, catching error case
void UnmountCallback(DADiskRef disk, DADissenterRef dissenter, void *context)
{
	UnMountState* pUnMountCalled = (UnMountState*) context;
	CFStringRef ErrorString;
	
	if ( pUnMountCalled )
	{
		// Handle error case
		if ( dissenter )
		{
			*pUnMountCalled = UnMountError;

			// Sometimes the dissenter will return a string, if it is available, show it to the user
			ErrorString = DADissenterGetStatusString(dissenter);
			if ( ErrorString )
				debugError("Unmount failed: %s\n", CFStringGetCStringPtr(ErrorString, kCFStringEncodingMacRoman));		
			else
				debug("Unmount failed\n");
		}
		else
			*pUnMountCalled = UnMountOK;		// UnMount was successful
	}
	else
		debugError("Invalid parameter in unmount callback\n");
}


// Print information about targets
void print_target_info(int nNumber, AoEDriverInterface* pInterface, AoEProperties* pProperties)
{
	CFStringRef		BSDName;
	CFStringRef pConfigString;
	char* pszCString;
	int nI;
	TargetInfo TInfo;

	if ( pInterface && (0 == pInterface->get_target_info(nNumber, &TInfo)) )
	{
		BSDName = pProperties->get_targets_bsd_name(TInfo.nTargetNumber);
		
		fprintf(stdout, "Target[%d] - Shelf=%d Slot=%d", TInfo.nTargetNumber, TInfo.nShelf, TInfo.nSlot);
		fprintf(stdout, " Capacity=%.0fMB", TInfo.NumSectors*512.0/(1024*1024));
		fprintf(stdout, " Sectors=%u\n", TInfo.NumSectors);
		if ( BSDName )
			fprintf(stdout, "          - BSD Name = \"%s\"\n", CFStringGetCStringPtr(BSDName, CFStringGetFastestEncoding(BSDName)));
		
		pConfigString = pProperties->get_targets_config_string(TInfo.nTargetNumber);
		
		pszCString = (char*) malloc(MAX_CONFIG_STRING_LENGTH);
		if ( pszCString && pConfigString )
			CFStringGetCString (pConfigString, pszCString, MAX_CONFIG_STRING_LENGTH, kCFStringEncodingMacRoman);
		
		fprintf(stdout, "          - Config String = \"%s\"\n", pszCString);
		
		if ( pConfigString )
			CFRelease(pConfigString);
		pConfigString = NULL;
		
		if ( pszCString )
			free(pszCString);
		pszCString = NULL;

		if ( BSDName )
			CFRelease(BSDName);
		BSDName = NULL;

		for (nI=0; nI<TInfo.nNumberOfInterfaces; nI++)
			fprintf(stdout, "          - Interface [en%d] Src %#x:%#x:%#x:%#x:%#x:%#x  Dest %#x:%#x:%#x:%#x:%#x:%#x\n", TInfo.aInterfaceNum[nI], TInfo.aaSrcMACAddress[nI][0], TInfo.aaSrcMACAddress[nI][1], TInfo.aaSrcMACAddress[nI][2], TInfo.aaSrcMACAddress[nI][3], TInfo.aaSrcMACAddress[nI][4], TInfo.aaSrcMACAddress[nI][5], TInfo.aaDestMACAddress[nI][0], TInfo.aaDestMACAddress[nI][1], TInfo.aaDestMACAddress[nI][2], TInfo.aaDestMACAddress[nI][3], TInfo.aaDestMACAddress[nI][4], TInfo.aaDestMACAddress[nI][5]);
		if ( !TInfo.nNumberOfInterfaces )
			fprintf(stdout, "          - Interface OFFLINE\n");
	}
}

int main (int argc,  char** argv)
{
	EthernetDetect eth;
	int anEthernetPorts[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	AoEProperties Properties;
	AoEPreferences Prefs;
	bool fSaveOptions;
	bool fWaitForKEXTToLoad;
	bool fSetOptionsInKEXT;
	int nOpt;
	DASessionRef	DiskSession;
	CFStringRef		BSDName;
	DADiskRef		Disk;
	
	DiskSession = DASessionCreate(kCFAllocatorDefault);
	DASessionScheduleWithRunLoop(DiskSession, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	
	// Setup defaults
	fSaveOptions = TRUE;		
	fSetOptionsInKEXT = TRUE;
	fWaitForKEXTToLoad = FALSE;
	BSDName = NULL;
	Disk = NULL;

	// Regardless of the configuration, make sure the ethernet ports have already been registered
	// This is only really an issue on startup: we need to ensure these ports are available before
	// we initialise the aoe driver. Not having this ready also affects the default initialisation
	// if there is no preference file installed
	eth.configure_matching();
	while ( 0 !=  eth.configure_complete() )
	{
		// Run our run loop briefly, waiting for a match
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
		debug("Ethernet interfaces not available. Waiting for it to appear...\n");
	}

	// Recall our settings before we start.
	// The options may choose to override some or all of these
	Prefs.recall_settings();

	if ( (0!=Properties.configure_matching()) || (0!=Properties.configure_complete()) )
		fprintf(stderr, "Unable to find device's properties\n");
	
	while ((nOpt = getopt(argc, argv, ":c:C:De:hi:l:psu:wx:")) != -1)
	{
		switch ( nOpt )
		{
			case 'c':
			{
				AoEDriverInterface Interface;
				ConfigString CStringInfo;
				char* pszNumber;
				int n, nTarget;
				
				if ( 0 != Interface.connect_to_driver() )
					fprintf(stderr, "Unable to connect to driver\n");
				
				// Targets to claim are passed in comma separated list
				pszNumber = strtok(optarg, ",");
				for (n=0; pszNumber != 0; n++, pszNumber = strtok(NULL, ","))
				{
					nTarget = strtol(pszNumber, NULL, 10);
					
					CStringInfo.nTargetNumber = nTarget;
					get_unique_config_string(CStringInfo.pszConfig, MAX_CONFIG_STRING_LENGTH);
					CStringInfo.Length = strlen(CStringInfo.pszConfig);
					debugVerbose("CLAIM--Target: %d, Size=%d, String=%s\n", nTarget, CStringInfo.Length, CStringInfo.pszConfig);
					
					if ( 0!=Interface.set_config_string(&CStringInfo) )
						fprintf(stderr, "Failed to set config string\n");
				}
				Interface.disconnect();
				break;
			}
			case 'C':
			{
				AoEDriverInterface Interface;
				ConfigString CStringInfo;
				char* pszNumber;
				UnMountState UMState;
				int n, nTarget;
				
				if ( 0 != Interface.connect_to_driver() )
					fprintf(stderr, "Unable to connect to driver\n");
				
				// Targets to claim are passed in comma separated list
				pszNumber = strtok(optarg, ",");
				for (n=0; pszNumber != 0; n++, pszNumber = strtok(NULL, ","))
				{
					UMState = UnMountIncomplete;
					
					nTarget = strtol(pszNumber, NULL, 10);
				
					// Get BSD Name
					BSDName = Properties.get_targets_bsd_name(nTarget);
					if ( BSDName )
					{
						Disk = DADiskCreateFromBSDName(kCFAllocatorDefault, DiskSession, CFStringGetCStringPtr(BSDName, kCFStringEncodingMacRoman));
						
						// If the disk is claimed, unmount it now
						if ( Disk )
						{
							debugVerbose("Unmount- %s Target: %d\n", CFStringGetCStringPtr(BSDName, kCFStringEncodingMacRoman), nTarget);

							DADiskUnmount(Disk, kDADiskUnmountOptionWhole, UnmountCallback, &UMState);
							
							// Wait for the unmount to complete
							while ( UMState == UnMountIncomplete )
								CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
						}
					}
					
					// Remove config string if unmount was successful
					if ( UMState!=UnMountError )
					{	
						CStringInfo.nTargetNumber = nTarget;
						CStringInfo.Length = 0;
						
						debugVerbose("UNCLAIM-- Target: %d, Size=%d\n", nTarget, CStringInfo.Length);
						
						if ( 0!=Interface.set_config_string(&CStringInfo) )
							fprintf(stderr, "Failed to set config string\n");
					}
					else
					{
						// Display alert if unmount failed
						CFUserNotificationDisplayAlert(0, kCFUserNotificationNoDefaultButtonFlag, NULL, NULL, NULL, CFSTR("AoE alert"), CFSTR("It is not possible to unmount the drive at this time"), CFSTR("Cancel"), NULL, NULL, NULL);
					}
				}
				Interface.disconnect();
				
				break;
			}
			case 'D':
			{
				AoEDriverInterface Interface;
				TargetInfo TInfo;
				
				// Just getting the targets info will force a broadcast
				Interface.get_target_info(1, &TInfo);
				break;
			}
			case 'e':
			{
				char* pszNumber;
				int n;
				
				pszNumber = strtok(optarg, ",");
				for (n=0; pszNumber != 0; n++, pszNumber = strtok(NULL, ","))
				{
					anEthernetPorts[n] = strtol(pszNumber, NULL, 10);
					if ( (anEthernetPorts[n]==0)  && (errno==EINVAL) )
					{
						fprintf(stderr, "Illegal port number\n");
						abort();
						break;
					}
					
					//printf("Found port: %d\n", anEthernetPorts[n]);
				}
				
				Prefs.set_available_ports(n, anEthernetPorts);
				break;
			}				
			case 'h':
			{
				fprintf(stdout, "usage: AoEd [-e [PORT]] [-c TARGET] [-C TARGET] [-D] [-h] [-i TARGET] [-p] [-s] [-u SIZE] [-w] [-x SIZE]\n");
				fprintf(stdout, "\n");
				fprintf(stdout, "c: Claim TARGET\n");
				fprintf(stdout, "C: Unclaim TARGET (clears config string)\n");
				fprintf(stdout, "D: Discover new devices \n");
				fprintf(stdout, "e: comma seperated list of ethernet port numbers to enable for AoE (eg -e0,1 would enable en0 and en1)\n");
				fprintf(stdout, " : without an argument, \"-e\" disables all ethernet ports\n");
				fprintf(stdout, "h: display this help\n");
				fprintf(stdout, "i: Information on AoE TARGET (or all if TARGET is not supplied)\n");
				fprintf(stdout, "p: display preference file\n");
				fprintf(stdout, "s: don't save options in preference file\n");
				fprintf(stdout, "x: Outstanding transfer size (kb)\n");
				fprintf(stdout, "u: User defined maximum bufffer count\n");
				fprintf(stdout, "w: wait for kext to load and accept settings before exiting\n");		// TODO: Add optional timeout?
				fSetOptionsInKEXT = FALSE;
				break;
			}
			case 'i':
			{
				AoEDriverInterface Interface;
				char* pszNumber;
				int n, nTarget;

				if ( 0==Interface.connect_to_driver() )
				{	
					// Targets are passed in comma separated list
					pszNumber = strtok(optarg, ",");
					for (n=0; pszNumber != 0; n++, pszNumber = strtok(NULL, ","))
					{
						nTarget = strtol(pszNumber, NULL, 10);

						print_target_info(nTarget, &Interface, &Properties);
					}
				}
				
				fSetOptionsInKEXT = FALSE;
				break;
			}
			case 'l':
			{
				int nLog = 0;
				
				if ( optarg )
					nLog = strtol(optarg, NULL, 10);
				
				AoEDriverInterface Interface;
				if ( 0==Interface.connect_to_driver() )
				{
					Interface.enable_logging(&nLog);
					Interface.disconnect();
				}
				break;
			}
			case 'p':
				Prefs.PrintPreferences();
				break;
			case 's':
				fSaveOptions = FALSE;
				break;
			case 'u':
			{
				int nSize = 1;
				
				if ( optarg )
					nSize = strtol(optarg, NULL, 10);
				
				Prefs.set_user_buffer_size(nSize);
				break;
			}
			case 'x':
			{
				int nSize = 1;
				
				if ( optarg )
					nSize = strtol(optarg, NULL, 10);
				
				Prefs.set_max_outstanding_size(nSize*1024);
				break;
			}
			case 'w':
				fWaitForKEXTToLoad = TRUE;
				break;
			case ':':
			{
				// Handle any characters that have optional arguments that are not supplied
				switch ( optopt )
				{
					case 'e':
					{
						// Disable all ports
						Prefs.set_available_ports(0, NULL);
						break;
					}
					case 'i':
					{
						UInt32 PayloadSize;
						int n, nValue, nTargets;
						ErrorInfo	Errs;
						AoEDriverInterface Interface;
						int nNumOfEthernetPorts;
						char acEthernetName[100];
						
						if ( 0==Interface.connect_to_driver() )
						{	
							if ( 0 != Interface.count_targets(&nTargets) )
								fprintf(stderr, "Trouble getting target count %d\n", nTargets);
							
							fprintf(stdout, "Found %d target(s)\n", nTargets);
							
							for (n=0; n<nTargets; n++)
								print_target_info(n+1, &Interface, &Properties);
							
							// Print information about our interfaces
							fprintf(stdout, "\n");
							nNumOfEthernetPorts = 0;
							for(n=0; n<eth.GetNumberOfInterfaces(); n++)
								if ( 0==Properties.get_en_interfaces(n, &nValue) )
									nNumOfEthernetPorts++;
							
							fprintf(stdout, "%d interface(s) enabled: ", nNumOfEthernetPorts);
							
							for(n=0; n<nNumOfEthernetPorts; n++)
							{
								if ( 0==Properties.get_en_interfaces(n, &nValue) )
								{
									eth.GetInterfaceName(nValue, acEthernetName, sizeof(acEthernetName));
									fprintf(stdout, "%s ", acEthernetName);
								}
							}
							fprintf(stdout, "\n");
							
							// Print info about payload size (based on MTU of all enabled interfaces)
							Interface.get_payload_size(&PayloadSize);
							fprintf(stdout, "AoE Payload size = %d bytes\n", PayloadSize);
							
							// Print Error info
							Interface.get_error_info(&Errs);
							fprintf(stdout, "%d Retransmits and %d unexpected responses on interfaces\n", Errs.nRetransmits, Errs.nUnexpectedResponses);
							Interface.disconnect();
						}
						
						fSetOptionsInKEXT = FALSE;
						break;
					}						
					case 'c':
					case 'C':
					case 'u':
					case 'x':
					{
						fprintf(stderr, "Options required for this argument\n");
						// fall-thru
					}
					default:
						abort();
				}
				break;
			}
			default:
				abort();
		}
	}
	
	if ( DiskSession )
		CFRelease(DiskSession);
	if ( BSDName )
		CFRelease(BSDName);
	if ( Disk )
		CFRelease(Disk);
	
	// Save our preferences to the file
	if ( fSaveOptions )
		if ( 0 != Prefs.store_settings() )
		{
			debugError("Unable to save settings file\n");
			return EXIT_FAILURE;
		}
	
	// Wait for our connection
	if ( fWaitForKEXTToLoad )
	{
		Properties.configure_matching();
		while ( 0 !=  Properties.configure_complete() )
		{
			// Run our run loop briefly, waiting for a match
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
			debugVerbose("AoE filter not available. Waiting for it to appear...\n");
		}
		debugVerbose("AoE filter now AVAILABLE.\n");
	}
	
	// Send the information to the kext
	if ( fSetOptionsInKEXT )
		if ( 0 != Prefs.SetSettingsInKEXT() )
		{
			debugError("Unable to communicate with KEXT\n");
			return EXIT_FAILURE;
		}
	
    return EXIT_SUCCESS;
}
