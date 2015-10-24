/*
 *  PreferenceLoadSave.cpp
 *  AoEd
 *
 *  This class handles the loading/saving of the preference file based on the preference structure
 *  It does not require any privileges to run
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include "PreferenceLoadSave.h"
#include "debug.h"


// These defines can be used to control backwards compatibility
#define CURRENT_SUPPORTED_SETTINGS_FILEVERSION		1
#define NEWEST_SUPPORTED_SETTINGS_FILEVERSION		1
#define OLDEST_SUPPORTED_SETTINGS_FILEVERSION		1

// Key names
#define SETTINGS_FILEVERSION		"Version"
#define SETTINGS_NUMBEROFPORTS		"NumberOfPorts"
#define SETTINGS_AVAILABLEPORTS		"AvailablePorts"
#define SETTINGS_TRANSFER_SIZE		"TransferSize"
#define SETTINGS_USER_BLOCK_COUNT	"MaxUserBlockCount"

// Actual path of our property list
static CFStringRef g_SettingsFileName = CFSTR("/Library/Preferences/net.corvus.AoEd.plist");


// Stores our preference file
int PreferenceLoadSave::store_settings(AoEPreferencesStruct* pPStruct)
{
	int n;
	// Read in the current settings file
	CFMutableDictionaryRef settingsDict = NULL;
	
	// the file was empty so we need to create a new dictionary and array
	settingsDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	
	//-----------------------------------------//
	// Store the Preference struct in the file //
	//-----------------------------------------//
	
	// Store the version number of the file
	int nSettingsFileVersion = CURRENT_SUPPORTED_SETTINGS_FILEVERSION;
	CFNumberRef nrefSettingsFileVersion = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &nSettingsFileVersion);
	CFDictionaryAddValue(settingsDict, CFSTR(SETTINGS_FILEVERSION), nrefSettingsFileVersion);
	CFRelease(nrefSettingsFileVersion);
	
	// Number of ports
	CFNumberRef nrefNumberOfPorts = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pPStruct->nNumberOfPorts);
	CFDictionaryAddValue(settingsDict, CFSTR(SETTINGS_NUMBEROFPORTS), nrefNumberOfPorts);
	CFRelease(nrefNumberOfPorts);
	
	// Array of available ports
	CFMutableArrayRef Ports = CFArrayCreateMutable(kCFAllocatorDefault, pPStruct->nNumberOfPorts, NULL);
	for (n=0; n<pPStruct->nNumberOfPorts; n++)
	{
		CFNumberRef nrefPortNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pPStruct->anEnabledPorts[n]);
		if ( nrefPortNumber )
		{
			CFArrayAppendValue(Ports, nrefPortNumber);
			CFRelease(nrefPortNumber);
		}
	}
	
	CFDictionaryAddValue(settingsDict, CFSTR(SETTINGS_AVAILABLEPORTS), Ports);
	CFRelease(Ports);
	
	// Transfer size
	CFNumberRef nrefTransferSize = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pPStruct->nMaxTransferSize);
	CFDictionaryAddValue(settingsDict, CFSTR(SETTINGS_TRANSFER_SIZE), nrefTransferSize);
	CFRelease(nrefTransferSize);

	// User block count
	CFNumberRef nrefBlockCount = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pPStruct->nUserBlockCountWindow);
	CFDictionaryAddValue(settingsDict, CFSTR(SETTINGS_USER_BLOCK_COUNT), nrefBlockCount);
	CFRelease(nrefBlockCount);
	
	// Write to the file
	CFURLRef outURLRef = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, g_SettingsFileName, kCFURLPOSIXPathStyle,false);
	if ( !outURLRef )
	{
		debugError("Trouble creating System path\n");
		return -1;
	}
	if ( !settingsDict ) 
	{
		debugError("Trouble creating settings dictionary\n");
		CFRelease(outURLRef);
		return -1;
	}
	
	CFDataRef xmlDataRef = CFPropertyListCreateXMLData(kCFAllocatorDefault, settingsDict);
	if ( !xmlDataRef )
	{
		debugError("Trouble creating XML data\n");
		CFRelease(settingsDict);
		CFRelease(outURLRef);
		return -1;
	}
	SInt32 nErrorCode;
	bool fSaveOK = CFURLWriteDataAndPropertiesToResource(outURLRef,xmlDataRef,NULL,&nErrorCode);
	
	if( !fSaveOK || nErrorCode)
		debugError("Trouble saving settings file at %s (error=%d)\n", CFStringGetCStringPtr(g_SettingsFileName, kCFStringEncodingMacRoman), nErrorCode);
	
	CFRelease(outURLRef);
	CFRelease(settingsDict);
	CFRelease(xmlDataRef);
	return 0;
}


// Recalls our setting preference file from the system
int PreferenceLoadSave::recall_settings(AoEPreferencesStruct* pPStruct)
{
	int n;
	
	// Set default values in case the load fails:
	pPStruct->nMaxTransferSize = DEFAULT_MAX_TRANSFER_SIZE;
	pPStruct->nUserBlockCountWindow = DEFAULT_CONGESTION_WINDOW;
	pPStruct->nNumberOfPorts = EthDetect.GetNumberOfInterfaces();
	for (n=0; n<pPStruct->nNumberOfPorts; n++)
		pPStruct->anEnabledPorts[n] = n;
	
	// Attempt to load the file
	CFURLRef inCFURLRef=CFURLCreateWithFileSystemPath(kCFAllocatorDefault, g_SettingsFileName, kCFURLPOSIXPathStyle, false);
	if ( !inCFURLRef )
		return -1;
	
	CFDataRef xmlCFDataRef;	
	CFPropertyListRef myCFPropertyListRef = NULL;
	Boolean status = CFURLCreateDataAndPropertiesFromResource( kCFAllocatorDefault, inCFURLRef, &xmlCFDataRef, NULL, NULL, NULL );
	if ( !status || !xmlCFDataRef )
		return -1;
	
	// Reconstitute the dictionary using the XML data.
	myCFPropertyListRef = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, xmlCFDataRef, kCFPropertyListImmutable, NULL );	
	if ( !myCFPropertyListRef )
		return -1;
	
	if ( CFGetTypeID(myCFPropertyListRef) == CFDictionaryGetTypeID() )
	{
		// Version handling
		CFDictionaryRef myDict = (CFDictionaryRef)myCFPropertyListRef;
		CFNumberRef nrefFileVersion;			
		CFDictionaryGetValueIfPresent(myDict, CFSTR(SETTINGS_FILEVERSION), (CFTypeRef*)&nrefFileVersion);
		if ( !nrefFileVersion )
		{
			debugError("Preference file version error\n");
			return -1;					
		}
		
		int nFileVersion;
		CFNumberGetValue(nrefFileVersion,kCFNumberIntType,&nFileVersion);
		
		if ( nFileVersion<OLDEST_SUPPORTED_SETTINGS_FILEVERSION )
		{
			debugError("only settings file sup to V%d are supported, file is V%d\n", OLDEST_SUPPORTED_SETTINGS_FILEVERSION, nFileVersion);
			debugError("Removing preference file\n");
			
			// delete the old preference file as it is no longer valid
			const char* szFileName = CFStringGetCStringPtr(g_SettingsFileName, kCFStringEncodingMacRoman);
			const int nMaxSize = strlen(szFileName) + strlen("rm ") + 1;
			char* szRunString = (char*)malloc(nMaxSize);
			snprintf(szRunString, nMaxSize, "rm %s", szFileName);
			system(szRunString);
			free(szRunString);
			return -1;
		}
		
		if ( nFileVersion>NEWEST_SUPPORTED_SETTINGS_FILEVERSION )
		{
			debugError("Preference file version error\n");
			return -1;
		}
		
		// Number of ports
		CFNumberRef nrefPorts;
		if ( CFDictionaryGetValueIfPresent(myDict, CFSTR(SETTINGS_NUMBEROFPORTS), (CFTypeRef*)&nrefPorts) )
		{
			if ( nrefPorts )
				CFNumberGetValue(nrefPorts, kCFNumberIntType, &pPStruct->nNumberOfPorts);
		}
		else
		{
			pPStruct->nNumberOfPorts = 0;
		}
		
		// Transfer size
		CFNumberRef nrefTransferSize;
		if ( CFDictionaryGetValueIfPresent(myDict, CFSTR(SETTINGS_TRANSFER_SIZE), (CFTypeRef*)&nrefTransferSize) )
		{
			if ( nrefTransferSize )
				CFNumberGetValue(nrefTransferSize, kCFNumberIntType, &pPStruct->nMaxTransferSize);
		}
		else
		{
			pPStruct->nMaxTransferSize = DEFAULT_MAX_TRANSFER_SIZE;
		}

		// Max Block count
		CFNumberRef nrefBlockCount;
		if ( CFDictionaryGetValueIfPresent(myDict, CFSTR(SETTINGS_USER_BLOCK_COUNT), (CFTypeRef*)&nrefBlockCount) )
		{
			if ( nrefBlockCount )
				CFNumberGetValue(nrefBlockCount, kCFNumberIntType, &pPStruct->nUserBlockCountWindow);
		}
		else
		{
			pPStruct->nUserBlockCountWindow = DEFAULT_CONGESTION_WINDOW;
		}
		
		// Array of available ports
		CFArrayRef ArrayPorts;			
		if ( CFDictionaryGetValueIfPresent(myDict, CFSTR(SETTINGS_AVAILABLEPORTS), (CFTypeRef*)&ArrayPorts) )
		{
			for (n=0; n<pPStruct->nNumberOfPorts; n++)
			{
				CFNumberRef nrefPort = (CFNumberRef) CFArrayGetValueAtIndex(ArrayPorts, n);
				if ( nrefPort )
					CFNumberGetValue(nrefPort, kCFNumberIntType, &pPStruct->anEnabledPorts[n]);
				
			}
			CFRelease(ArrayPorts);
		}
	}
	
	return 0;
}
