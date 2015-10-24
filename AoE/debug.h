/*
	debug.h 
	Macros to enable/disable different levels of debug output
 
	Copyright © 2010 Brantley Coile Company, Inc. All rights reserved.
 */

#include <sys/cdefs.h>

#ifdef DEBUGBUILD
#define USE_FIRELOG
//#define USE_IOLOG
#else
#define USE_IOLOG
#endif

#ifdef USE_IOLOG
#include <IOKit/IOLib.h>
#endif

#ifdef USE_FIRELOG
//#include <IOKit/firewire/FireLog.h>
// Although the header is installed in the framework directory, it's not in the 10.4u SDK that we're building against, so we can't reference it here.
// To work around it, i've just imported the prototype function here

__BEGIN_DECLS
extern void FireLog( const char *format, ... );
__END_DECLS
#endif

#if defined(USE_FIRELOG) && defined(USE_IOLOG)  // both FireLog and iolog

#define debugVerbose(args...)	do { FireLog("AoE: " args); IOLog("AoE: " args);} while(FALSE)
#define debugWarn(args...)		do { FireLog("AoE: " args); IOLog("AoE: " args);} while(FALSE)
#define debugError(args...)		do { FireLog("AoE *Error* : " args); IOLog("AoE *Error* : " args);} while(FALSE)
#define debug(args...)			do { FireLog("AoE: " args); IOLog("AoE: " args);} while(FALSE)

#define debugShort(args...)		do { FireLog(args); IOLog(args);} while(FALSE)

#elif defined(USE_FIRELOG)  // just FireLog

#define debugVerbose(args...)	do { FireLog("AoE: " args); } while(FALSE)
#define debugWarn(args...)		do { FireLog("AoE: " args); } while(FALSE)
#define debugError(args...)		do { FireLog("AoE *Error* : " args); } while(FALSE)
#define debug(args...)			do { FireLog("AoE: " args); } while(FALSE)

#define debugShort(args...)		do { FireLog(args); } while(FALSE)

#elif defined(USE_IOLOG)  // just iolog

#ifdef DEBUG
#define debugVerbose(args...)	do { IOLog("AoE: " args);} while(FALSE)
#define debug(args...)			do { IOLog("AoE: " args);} while(FALSE)
#define debugShort(args...)		do { IOLog(args); } while(FALSE)
#else
#define debugVerbose(args...)	do {;} while(FALSE)
#define debug(args...)			do {;} while(FALSE)
#define debugShort(args...)		do {;} while(FALSE)
#endif

#define debugWarn(args...)		do { IOLog("AoE: " args);} while(FALSE)
#define debugError(args...)		do { IOLog("AoE *Error* : " args);} while(FALSE)

#else	// no logging

#define debugVerbose(args...)	do {;} while(FALSE)
#define debugWarn(args...)		do {;} while(FALSE)
#define debugError(args...)		do {;} while(FALSE)
#define debug(args...)			do {;} while(FALSE)
#define debugShort(args...)		do {;} while(FALSE)

#endif
