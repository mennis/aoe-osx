/* debug.h 
   Macros to enable/disable different levels of debug output
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 */

#ifndef __DEBUG_H__
#define __DEBUG_H__

#include <stdio.h>

#ifndef DEBUG
#define debugVerbose(args...)	do {;} while(0)
#define debug(args...)			do {;} while(0)
#else
#define debugVerbose(args...)	do { printf("AoEPreferences: " args);} while(0)
#define debug(args...)			do { printf("AoEPreferences: " args);} while(0)
#endif

#define debugWarn(args...)		do { printf("AoEPreferences: " args);} while(0)
#define debugError(args...)		do { printf("AoEPreferences *Error* : " args);} while(0)

#endif		//__DEBUG_H__
