/*
 *  Preferences.cpp
 *  AoEd
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

/*
 * To create additonal preferences:
 *
 * 1/ Modify the AoEPreferenceStruct to include the preferences
 * 2/ Add support in AoEPreferences to store/recall the preferences from the file to the struct
 * 3/ Add support in SetSettingsInKEXT to send struct to the KEXT
 * 4/ Add support in KEXT to handle new structure items
 *
 */
 
#include "Preferences.h"
#include "AoEDriverInterface.h"
#include "PreferenceLoadSave.h"
#include "ConfigString.h"
#include "Debug.h"

AoEPreferences::AoEPreferences()
{
	m_pInterface = new AoEDriverInterface;
	m_pPrefLoadSave = new PreferenceLoadSave;
	
	// NOTE: the Config string isn't stored in the preference file (there's no need as it's purely determined by the computer)
	get_unique_config_string((char*)m_PreferenceData.aszComputerConfigString, sizeof(m_PreferenceData.aszComputerConfigString));
}

AoEPreferences::~AoEPreferences()
{
	delete m_pInterface;
	delete m_pPrefLoadSave;
}

void AoEPreferences::set_available_ports(int nNumberOfPorts, int* pnPorts)
{
	int n;

	// Check we dont overrun the buffer
	if ( nNumberOfPorts>MAX_SUPPORTED_ETHERNET_CONNETIONS )
		nNumberOfPorts = MAX_SUPPORTED_ETHERNET_CONNETIONS;
	
	m_PreferenceData.nNumberOfPorts = nNumberOfPorts;

	for(n=0; n<nNumberOfPorts; n++)
		m_PreferenceData.anEnabledPorts[n] = pnPorts[n];
}

void AoEPreferences::set_max_outstanding_size(int nSize)
{
	m_PreferenceData.nMaxTransferSize = nSize;
}

void AoEPreferences::set_user_buffer_size(int nSize)
{
	m_PreferenceData.nUserBlockCountWindow = nSize;
}

// Display all the preference on the stdout
void AoEPreferences::PrintPreferences(void)
{
	int n;
	
	fprintf(stdout, "NumberOfPorts = %d\n", m_PreferenceData.nNumberOfPorts);
	for(n=0; n< m_PreferenceData.nNumberOfPorts; n++)
		fprintf(stdout, "EnabledPort[%d] = en%d\n", n, m_PreferenceData.anEnabledPorts[n]);
	
	fprintf(stdout, "Transfer buffers = %dkb\n", m_PreferenceData.nMaxTransferSize);
	fprintf(stdout, "User Block Count = %d\n", m_PreferenceData.nUserBlockCountWindow);
	fprintf(stdout, "Computers config string = \"%s\"\n", m_PreferenceData.aszComputerConfigString);
}

int AoEPreferences::SetSettingsInKEXT(void)
{
	int nRet;
	
	nRet = m_pInterface->connect_to_driver();
	if ( nRet )
	{
		debugVerbose("Unable to connect to driver\n");
		return nRet;
	}

	nRet = m_pInterface->set_preference_settings(&m_PreferenceData);
	if ( nRet )
	{
		debugError("Unable to set settings in driver\n");
		return nRet;
	}
	
	m_pInterface->disconnect();
	return 0;
}


int AoEPreferences::recall_settings(void)
{
	return m_pPrefLoadSave->recall_settings(&m_PreferenceData);
}

int AoEPreferences::store_settings(void)
{
	return m_pPrefLoadSave->store_settings(&m_PreferenceData);
}
