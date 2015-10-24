/*
 *  EthernetDetect.cpp
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkInterface.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/IOBSD.h>
#include "EthernetDetect.h"
#include "debug.h"

EthernetDetect::EthernetDetect()
{
}

EthernetDetect::~EthernetDetect()
{
}

// Classes to this class should release the Iterator when they are done with it
void EthernetDetect::GetEthernetClasses(io_iterator_t* pIterator)
{
    kern_return_t			kernResult;
    CFMutableDictionaryRef	matchingDict;
    
    // Create a dictionary of all ethernet classes in the system
    matchingDict = IOServiceMatching(kIOEthernetInterfaceClass);
	
    // IOServiceGetMatchingServices retains the returned iterator, so release the iterator when we're done with it. This is done in the destructor
    // IOServiceGetMatchingServices also consumes a reference on the matching dictionary so we don't need to release
    // the dictionary explicitly.
    kernResult = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, pIterator);    
    if (KERN_SUCCESS != kernResult)
        debugError("IOServiceGetMatchingServices returned 0x%08x\n", kernResult);
}



int EthernetDetect::GetNumberOfInterfaces(void)
{
	io_iterator_t	Iterator;
	io_object_t		intService;
	int				nNumberOfInterfaces;

	GetEthernetClasses(&Iterator);
	
	if ( Iterator )
	{
		nNumberOfInterfaces = 0;
		// IOIteratorNext retains the returned object, so release it when we're done with it.
		while ((intService = IOIteratorNext(Iterator)))
		{
			++nNumberOfInterfaces;
			IOObjectRelease(intService);
		}
		
		IOObjectRelease(Iterator);
	}
	
	return nNumberOfInterfaces;
}

void EthernetDetect::GetInterfaceName(int nNumber, char* pszBSDName, int nMaxSize)
{
	io_iterator_t	Iterator;
	io_object_t		intService;
	
	GetEthernetClasses(&Iterator);
	int nCount = 0;

	// IOIteratorNext retains the returned object, so release it when we're done with it.
	while ((intService = IOIteratorNext(Iterator)))
	{
		if ( nCount==nNumber )
		{
			CFStringRef BSDName=(CFStringRef)IORegistryEntryCreateCFProperty(intService,CFSTR(kIOBSDNameKey),kCFAllocatorDefault,0);
			if ( BSDName )
			{
				CFStringGetCString(BSDName, pszBSDName, nMaxSize, kCFStringEncodingASCII);
				CFRelease(BSDName);
			}
			IOObjectRelease(intService);			
			break;
		}

		IOObjectRelease(intService);
		++nCount;
	}
	IOObjectRelease(Iterator);
}
