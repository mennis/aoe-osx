/*
 *  ConfigString.h
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __CONFIG_STRING_H__
#define __CONFIG_STRING_H__

__BEGIN_DECLS

__private_extern__ void get_unique_config_string(char* pszCString, int nSize);
__private_extern__ const char* get_owner_from_cstr(const char* pszCString);

__private_extern__ int cstr_ours(const char* pszCString);

__END_DECLS

#endif		//__CONFIG_STRING_H__
