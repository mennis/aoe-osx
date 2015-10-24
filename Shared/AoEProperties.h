/*
 *  AoEProperties.h
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOEPROPERTIES_H__
#define __AOEPROPERTIES_H__

#include <IOKit/IOKitLib.h>
#include "AoEcommon.h"

class AoEProperties
{
public:
	AoEProperties();
	virtual ~AoEProperties();

	int configure_matching(void);
	int configure_complete(void);
	int get_en_interfaces(int nNumber, int* pENInterfaces);
	int number_of_targets(void);
	int get_shelf_number(int nNumber);
	int get_target_number(int nNumber);
	UInt64 get_capacity(int nNumber);
	int get_slot_number(int nNumber);
	int get_targets_en_interfaces(int nNumber, int* pENInterfaces);
	CFStringRef get_config_string(int nNumber);
	CFStringRef get_targets_config_string(int nTargetNumber);
	CFStringRef get_targets_bsd_name(int nTargetNumber);
private:
	static void matched_callback(void *refcon, io_iterator_t iterator );
	static void terminate_callback(void* pRefCon, io_iterator_t iterator );
	int get_property(CFTypeRef* pType, CFStringRef Property, int nNumber);

	static CFRunLoopSourceRef		ms_IOKitNotificationRunLoopSource;
	static IONotificationPortRef	ms_NotificationPort;

	static io_iterator_t			ms_MatchIt;

	bool							m_fMatched;
	io_registry_entry_t				m_OurObject;
};


#endif		// __AOEPROPERTIES_H__
