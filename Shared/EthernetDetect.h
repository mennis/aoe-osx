/*
 *  EthernetDetect.h
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __ETHERNET_DETECT_H__
#define __ETHERNET_DETECT_H__

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

class EthernetDetect
{
public:
	EthernetDetect();
	virtual ~EthernetDetect();

	int GetNumberOfInterfaces(void);
	void GetInterfaceName(int nNumber, char* pszBSDName, int nMaxSize);
private:
	void GetEthernetClasses(io_iterator_t* pIterator);
};

#endif	//__ETHERNET_DETECT_H__
