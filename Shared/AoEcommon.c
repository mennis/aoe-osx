/*
 *  AoEcommon.c
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#include <kern/clock.h>
#include <sys/types.h>
#include "AoEcommon.h"

/*---------------------------------------------------------------------------
 * Basic time functions
 ---------------------------------------------------------------------------*/

uint64_t time_since_now_ns(uint64_t old_time)
{
	uint64_t CurrentTime;
	uint64_t current_nano, old_nano;
	
	// Get time, convert to ns and return the difference
	clock_get_uptime(&CurrentTime);
	absolutetime_to_nanoseconds(CurrentTime, &current_nano);
	absolutetime_to_nanoseconds(old_time, &old_nano);
	
	return current_nano-old_nano;
}



uint64_t time_since_now_ms(uint64_t old_time)
{
	return CONVERT_NS_TO_MS(time_since_now_ns(old_time));
}



uint64_t time_since_now_us(uint64_t old_time)
{
	return CONVERT_NS_TO_US(time_since_now_ns(old_time));
}


