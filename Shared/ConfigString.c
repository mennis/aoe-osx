/*
 *  ConfigString.c
 *  AoEPreference
 *
 *  The config string is defined as: "PREFIX SERIALNUM NAME"
 *
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ConfigString.h"
#include "debug.h"

#define APPLE_DRIVER_PREFIX			"net.corvus.apple"

#define numberof(x)					(sizeof(x)/sizeof(x[0]))

// See: http://developer.apple.com/technotes/tn/tn1103.html

// Returns the serial number as a CFString.
// It is the caller's responsibility to release the returned CFString when done with it.
static void CopySerialNumber(CFStringRef *pSerialNumber)
{
    if (pSerialNumber != NULL)
	{
        *pSerialNumber = NULL;
		
        io_service_t platformExpert = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
		
        if ( platformExpert )
		{
            CFTypeRef serialNumberAsCFString = IORegistryEntryCreateCFProperty(platformExpert, CFSTR(kIOPlatformSerialNumberKey), kCFAllocatorDefault, 0);

            if ( serialNumberAsCFString )
                *pSerialNumber = serialNumberAsCFString;
			
            IOObjectRelease(platformExpert);
        }
    }
}

// Skip past the first two phrases separated by spaces. The last phrase (LUN owner) can contain spaces
const char* get_owner_from_cstr(const char* pszCString)
{
	int n, nLen;
	const char* pszOwner;
	static const char pszUnknownOwner[] = "Unknown";

	// Quick check that it's previously owned by our driver
	if ( 0 != strncmp(pszCString, APPLE_DRIVER_PREFIX, strlen(APPLE_DRIVER_PREFIX)) )
		return pszUnknownOwner;
	
	pszOwner = pszCString;
	for(n=0; n<2; n++)
	{
		nLen = strlen(pszOwner);

		while ( *pszOwner++!=' ' )
			if ( !--nLen )
				break;
	}
	
	return pszOwner;
}

// Check the serial number is the same as the computer we are now on
int cstr_ours(const char* pszCString)
{
	CFStringRef SerialNumber;
	CFStringRef SerialNumberInCString;
	CFRange		Rng;
	int			nRet;
	
	if ( NULL==pszCString )
		return -1;

	// Quick check that it's previously owned by our driver
	if ( 0 != strncmp(pszCString, APPLE_DRIVER_PREFIX, strlen(APPLE_DRIVER_PREFIX)) )
		return -1;
	
	// Check the computer serial number
	CopySerialNumber(&SerialNumber);
	Rng.location = 0;	
	Rng.length = CFStringGetLength(SerialNumber);

	SerialNumberInCString = CFStringCreateWithCString(NULL, pszCString+strlen(APPLE_DRIVER_PREFIX)+1, kCFStringEncodingUTF8);
	nRet = CFStringFindWithOptions(SerialNumberInCString, SerialNumber, Rng, 0, 0) ? 0 : -1;

	CFRelease(SerialNumberInCString);
	CFRelease(SerialNumber);

	return nRet;
}

// return a unique config string for this computer
void get_unique_config_string(char* pszCString, int nSize)
{
	CFStringRef strs[3];
	CFStringRef SerialNumber;
	CFStringRef MachineName;
	CFStringRef Full;
	CFArrayRef ArrayStrings;
	
	CopySerialNumber(&SerialNumber);
	MachineName = CSCopyMachineName();
	
	strs[0] = CFSTR(APPLE_DRIVER_PREFIX);
	strs[1] = SerialNumber;
	strs[2] = MachineName;
	
	ArrayStrings = CFArrayCreate(NULL, (void *)strs, numberof(strs), &kCFTypeArrayCallBacks);

	Full = CFStringCreateByCombiningStrings(NULL, ArrayStrings, CFSTR(" "));
	CFStringGetCString(Full, pszCString, nSize, kCFStringEncodingUTF8);

	CFRelease(SerialNumber);
	CFRelease(MachineName);
	CFRelease(Full);
}
