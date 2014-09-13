//
//  nbd.c
//  nbd
//
//  Created by Frank Laub on 9/13/14.
//  Copyright (c) 2014 JFInc. All rights reserved.
//

#include <mach/mach_types.h>

kern_return_t nbd_start(kmod_info_t * ki, void *d);
kern_return_t nbd_stop(kmod_info_t *ki, void *d);

kern_return_t nbd_start(kmod_info_t * ki, void *d)
{
    return KERN_SUCCESS;
}

kern_return_t nbd_stop(kmod_info_t *ki, void *d)
{
    return KERN_SUCCESS;
}
