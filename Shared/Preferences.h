/*
 *  Preferences.h
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_PREFERENCES_H__
#define __AOE_PREFERENCES_H__

#include "AoEcommon.h"

class AoEDriverInterface;
class PreferenceLoadSave;

class AoEPreferences
{
public:
	AoEPreferences();
	virtual ~AoEPreferences();

	int recall_settings(void);
	int store_settings(void);
	void set_available_ports(int nNumberOfPorts, int* pnPorts);
	void set_max_outstanding_size(int nSize);
	void set_user_buffer_size(int nSize);
	void PrintPreferences(void);

	int SetSettingsInKEXT(void);
private:
	// Interface to our kext
	AoEDriverInterface*		m_pInterface;
	PreferenceLoadSave*		m_pPrefLoadSave;

	AoEPreferencesStruct	m_PreferenceData;
	// Data
};

#endif		//__AOE_PREFERENCES_H__
