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
#define debugVerbose(args...)	do { fprintf(stdout, "AoEd: " args);} while(0)
#define debug(args...)			do { fprintf(stdout, "AoEd: " args);} while(0)
#endif

#define debugWarn(args...)		do { fprintf(stdout, "AoEd: " args);} while(0)
#define debugError(args...)		do { fprintf(stderr, "AoEd *Error* : " args);} while(0)

#endif		//__DEBUG_H__
