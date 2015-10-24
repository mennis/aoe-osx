/*
 *  AoEtherFilter.h
 *  AoEInterface
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_ETHER_FILTER_H__
#define __AOE_ETHER_FILTER_H__

#include <sys/kernel_types.h>

__BEGIN_DECLS

__private_extern__ kern_return_t filter_init(void);

__private_extern__ kern_return_t enable_filtering(int nEthernetNumber, ifnet_t Enetifnet);
__private_extern__ kern_return_t disable_filtering(int nEthernetNumber);

__private_extern__ void filter_uninit(void);

__private_extern__ void enable_filter_logging(int nEnableLogging);

__private_extern__ void set_filtering_controller(void* pController);

__END_DECLS

#endif		// __AOE_ETHER_FILTER_H__
