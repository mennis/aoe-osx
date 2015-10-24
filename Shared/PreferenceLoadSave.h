/*
 *  PreferenceLoadSave.h
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 */

#ifndef __AOE_PREFERENCE_LOAD_SAVE_H__
#define __AOE_PREFERENCE_LOAD_SAVE_H__

#include "AoEcommon.h"
#include "EthernetDetect.h"

class PreferenceLoadSave
{
public:
	int recall_settings(AoEPreferencesStruct* pPStruct);
	int store_settings(AoEPreferencesStruct* pPStruct);
private:
	EthernetDetect		EthDetect;
};


#endif		//__AOE_PREFERENCE_LOAD_SAVE_H__
