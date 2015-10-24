/*
 *  AoEUserInterface.h
 *  AoEInterface
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_USER_INTERFACE__
#define __AOE_USER_INTERFACE__

#include <sys/cdefs.h>

__BEGIN_DECLS

__private_extern__ void set_ui_controller(void* pController);

__private_extern__ int open_user_interface();
__private_extern__ int close_user_interface();

__END_DECLS

#endif		//__AOE_USER_INTERFACE__
