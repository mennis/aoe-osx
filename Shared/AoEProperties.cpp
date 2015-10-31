/*
 *  AoEProperties.cpp
 *  AoEd
 *
 *  This class is used to get info from the AoE kext.
 *  It does this through standard properties and can be called without any special permissions
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include "AoEProperties.h"
#include "AoEcommon.h"
#include "debug.h"
#include "Preferences.h"

IONotificationPortRef AoEProperties::ms_NotificationPort = NULL;
CFRunLoopSourceRef AoEProperties::ms_IOKitNotificationRunLoopSource = NULL;

io_iterator_t AoEProperties::ms_MatchIt = IO_OBJECT_NULL;

#pragma mark Construct/Destruct

AoEProperties::AoEProperties()
{
	m_fMatched = FALSE;
	m_OurObject = NULL;
}

AoEProperties::~AoEProperties()
{
	if ( ms_MatchIt )
		IOObjectRelease(ms_MatchIt);
	
	if ( m_OurObject )
		IOObjectRelease(m_OurObject);
}

#pragma mark --
#pragma mark Matching

int AoEProperties::configure_matching(void)
{
//	debugVerbose("AoEProperties::configure_matching\n");
	
	// Obtain ports for notifications (this will be used for all service matching notifications)
	kern_return_t kresult;
	mach_port_t MasterPort;
	kresult = IOMasterPort(MACH_PORT_NULL, &MasterPort);
	if ( kresult )
	{
		debugError("Could not get masterport. Error=%d\n", kresult);
		return false;
	}
	
	ms_NotificationPort = IONotificationPortCreate(MasterPort);
	
	ms_IOKitNotificationRunLoopSource = IONotificationPortGetRunLoopSource(ms_NotificationPort);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), ms_IOKitNotificationRunLoopSource, kCFRunLoopDefaultMode);
	
	// SetUp Notification for matching to our device
	CFMutableDictionaryRef MatchingDict = IOServiceMatching(AOE_KEXT_NAME_Q);

	IOServiceAddMatchingNotification(ms_NotificationPort,
									 kIOMatchedNotification,
									 MatchingDict,
									 AoEProperties::matched_callback,
									 this,
									 &ms_MatchIt);
	
	// Call the callback immediately to check if our iterator already contains our device (ie. the device is already loaded)
	matched_callback(this, ms_MatchIt);
	
	return m_fMatched ? 0 : -1;
}

void AoEProperties::matched_callback(void* pRefcon, io_iterator_t iterator)
{	
	io_registry_entry_t Object = IOIteratorNext(iterator);
	
	AoEProperties* pThis = (AoEProperties*) pRefcon;

	if ( Object )
	{
		//debug("AOEINTERFACE ONLINE!\n");
		if ( pThis )
			pThis->m_fMatched = TRUE;
	
		if ( pThis->m_OurObject )
			IOObjectRelease(pThis->m_OurObject);
		 
		 pThis->m_OurObject = Object;

		// Since we have matched, we remove our source from the run loop
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), ms_IOKitNotificationRunLoopSource, kCFRunLoopDefaultMode);
	}
	
	// Empty the remaining devices in the list (don't release the iterator though, or we won't get our callback)
	while( 0 != (Object=IOIteratorNext(iterator)) )
		IOObjectRelease(Object);
}

int AoEProperties::configure_complete(void)
{
	return m_fMatched ? 0 : -1;
}

int AoEProperties::number_of_targets(void)
{
	io_registry_entry_t ControllerInterface = NULL;
	io_iterator_t	Controllers = NULL;
	io_registry_entry_t Controller = NULL;
	int nCount = 0;
	
	if ( 0==m_OurObject )
		return 0;

	IORegistryEntryGetChildEntry(m_OurObject, kIOServicePlane, &ControllerInterface);

	IORegistryEntryGetChildIterator(ControllerInterface, kIOServicePlane, &Controllers);

	while ( (Controller = IOIteratorNext(Controllers)) )
	{
		IOObjectRelease(Controller);
		++nCount;
	}
	
	if ( Controllers )
		IOObjectRelease(Controllers);

	if ( ControllerInterface )
		IOObjectRelease(ControllerInterface);

	return nCount;
}

int AoEProperties::get_targets_en_interfaces(int nNumber, int* pENInterfaces)
{
	int n, nCount;
	CFArrayRef ENInterfaces;
	
	nCount = 0;

	if ( 0==get_property((CFTypeRef*)&ENInterfaces, CFSTR(ATTACHED_INTERFACES_PROPERTY), nNumber) )
	{
		nCount = CFArrayGetCount(ENInterfaces);
		for(n=0; n<nCount; n++)
		{
			CFNumberRef num = (CFNumberRef) CFArrayGetValueAtIndex(ENInterfaces, n);
			CFNumberGetValue(num, kCFNumberIntType, &pENInterfaces[n]);
			CFRelease(num);
		}

		CFRelease(ENInterfaces);
	}
	
	return nCount;
}

int AoEProperties::get_target_number(int nNumber)
{
	int nTarget;
	CFTypeRef Target;
	
	if ( 0==get_property(&Target, CFSTR(TARGET_PROPERTY), nNumber) )
	{
		CFNumberGetValue((CFNumberRef)Target, kCFNumberIntType, &nTarget);
		CFRelease(Target);
	}
	
	return nTarget;
}

int AoEProperties::get_shelf_number(int nNumber)
{
	int nShelf;
	CFTypeRef shelf;

	if ( 0==get_property(&shelf, CFSTR(SHELF_PROPERTY), nNumber) )
	{
		CFNumberGetValue((CFNumberRef)shelf, kCFNumberIntType, &nShelf);
		CFRelease(shelf);
	}

	return nShelf;
}

int AoEProperties::get_slot_number(int nNumber)
{
	int nSlot;
	CFTypeRef shelf;
	
	if ( 0==get_property(&shelf, CFSTR(SLOT_PROPERTY), nNumber) )
	{
		CFNumberGetValue((CFNumberRef)shelf, kCFNumberIntType, &nSlot);
		CFRelease(shelf);
	}
	
	return nSlot;
}

UInt64 AoEProperties::get_capacity(int nNumber)
{
	UInt64 Capacity;
	CFTypeRef shelf;
	
	if ( 0==get_property(&shelf, CFSTR(CAPACITY_PROPERTY), nNumber) )
	{
		CFNumberGetValue((CFNumberRef)shelf, kCFNumberLongLongType, &Capacity);
		CFRelease(shelf);
	}
	
	return Capacity;
}

CFStringRef AoEProperties::get_config_string(int nNumber)
{
	CFStringRef ConfigString;
	
	if ( 0==get_property((CFTypeRef*)&ConfigString, CFSTR(CONFIG_STRING_PROPERTY), nNumber) )
		return (CFStringRef) ConfigString;
	else
		return 0;
}

CFStringRef AoEProperties::get_targets_config_string(int nTargetNumber)
{
	int n, nNum;
	
	// Iterate over the list of targets, checking which one is the target number we are looking for
	nNum = number_of_targets();
	for(n=0; n<nNum; n++)
		if ( nTargetNumber==get_target_number(n) )
			return get_config_string(n);

	return NULL;
}

int AoEProperties::get_property(CFTypeRef* pType, CFStringRef Property, int nNumber)
{
	io_registry_entry_t ControllerInterface;
	io_iterator_t	Controllers;
	io_registry_entry_t Controller;
	int nCount;
	int nShelf;
	
	ControllerInterface = NULL;
	Controllers = NULL;
	Controller = NULL;
	nCount = 0;
	nShelf = 0;
	*pType = NULL;

	if ( 0==m_OurObject )
		return 0;
	
	IORegistryEntryGetChildEntry(m_OurObject, kIOServicePlane, &ControllerInterface);
	IORegistryEntryGetChildIterator(ControllerInterface, kIOServicePlane, &Controllers);
	
	while ( (Controller = IOIteratorNext(Controllers)) )
	{
		if ( nNumber==nCount )
		{	
			*pType = IORegistryEntryCreateCFProperty(Controller,Property,kCFAllocatorDefault,0);
			if ( *pType )
				CFRetain(*pType);

			IOObjectRelease(Controller);
			break;
		}
		
		IOObjectRelease(Controller);
		++nCount;
	}
	
	if ( Controllers )
		IOObjectRelease(Controllers);
	
	if ( ControllerInterface )
		IOObjectRelease(ControllerInterface);
	
	return (*pType && (nCount==nNumber)) ? 0 : -1;
}

CFStringRef AoEProperties::get_targets_bsd_name(int nTargetNumber)
{
	io_registry_entry_t ControllerInterface;
	io_iterator_t	Controllers;
	io_registry_entry_t Controller;
	io_registry_entry_t Device;
	io_registry_entry_t DiskDriver;
	io_registry_entry_t StorageDevice;
	io_registry_entry_t StorageDriver;
	io_registry_entry_t Disk;
	CFStringRef	Name;
	CFNumberRef Target;
	int nTargetNum;
	
	DiskDriver = NULL;
	StorageDevice = NULL;
	StorageDriver = NULL;
	Device = NULL;
	Disk = NULL;
	ControllerInterface = NULL;
	Controllers = NULL;
	Controller = NULL;
	Name = NULL;

	if ( 0==m_OurObject )
		return 0;
	
	IORegistryEntryGetChildEntry(m_OurObject, kIOServicePlane, &ControllerInterface);
	IORegistryEntryGetChildIterator(ControllerInterface, kIOServicePlane, &Controllers);
	
	// First, find the Controller...
	while ( (Controller = IOIteratorNext(Controllers)) )
	{
		Target = (CFNumberRef) IORegistryEntryCreateCFProperty(Controller, CFSTR(TARGET_PROPERTY), kCFAllocatorDefault, 0);
		CFNumberGetValue(Target, kCFNumberIntType, &nTargetNum);
		CFRelease(Target);

		if ( nTargetNumber==nTargetNum )
		{
			// Now we have the controller, descend through children
			IORegistryEntryGetChildEntry(Controller, kIOServicePlane, &Device);
			IORegistryEntryGetChildEntry(Device, kIOServicePlane, &DiskDriver);
			IORegistryEntryGetChildEntry(DiskDriver, kIOServicePlane, &StorageDevice);
			IORegistryEntryGetChildEntry(StorageDevice, kIOServicePlane, &StorageDriver);
			IORegistryEntryGetChildEntry(StorageDriver, kIOServicePlane, &Disk);

			if ( Disk )
			{
				Name = (CFStringRef) IORegistryEntryCreateCFProperty(Disk, CFSTR("BSD Name"), kCFAllocatorDefault, 0);

				if ( Name )
					CFRetain(Name);
			}
			break;
		}
		
		IOObjectRelease(Controller);
	}
	
	if ( Controllers )
		IOObjectRelease(Controllers);
	if ( DiskDriver )
		IOObjectRelease(DiskDriver);
	if ( StorageDevice )
		IOObjectRelease(StorageDevice);
	if ( StorageDriver )
		IOObjectRelease(StorageDriver);
	if ( Disk )
		IOObjectRelease(Disk);
	if ( Device )
		IOObjectRelease(Device);
	if ( ControllerInterface )
		IOObjectRelease(ControllerInterface);
	
	return (Name && (nTargetNum==nTargetNumber)) ? Name : NULL;
}

int AoEProperties::get_en_interfaces(int nNumber, int* pENInterfaces)
{
	CFArrayRef	EnInterfaces;
	CFNumberRef	Num;
	
	if ( NULL==pENInterfaces )
		return -1;

	EnInterfaces = (CFArrayRef) IORegistryEntryCreateCFProperty(m_OurObject, CFSTR(ENABLED_INTERFACES_PROPERTY), kCFAllocatorDefault, 0);
	
	if ( NULL==EnInterfaces )
		return -1;
	
	if ( nNumber >= CFArrayGetCount(EnInterfaces) )
		return -1;
	
	Num = (CFNumberRef) CFArrayGetValueAtIndex(EnInterfaces, nNumber);

	return CFNumberGetValue(Num, kCFNumberIntType, pENInterfaces) ? 0 : -1;	
}

// This is a generic handler for termination. It's passed a different iterator depending on the device type
void AoEProperties::terminate_callback(void* pRefCon, io_iterator_t iterator )
{
	debug("AOE DRIVER OFFLINE!\n");
}
