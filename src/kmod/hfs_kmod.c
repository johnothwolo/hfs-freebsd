//
//  hfs_kmod.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/16/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/smp.h>

#include "hfs_encodings/hfs_encodings_internal.h"
#include "core/hfs.h"
#include "core/hfs_cnode.h"

extern void darwin_compat_init(void);
extern void darwin_compat_uninit(void);

// hfs_bdg.h
#if DEBUG
bool hfs_corruption_panics = true;
#endif

static int hfs_kmod_modevent(struct module *inModule, int inEvent, void *inArg)
{
    // Set return code to 0
    int returnCode = 0;
    
    switch (inEvent)
    {
        case MOD_LOAD:
            uprintf("Loading hfs kmod \n");
            darwin_compat_init();
            hfs_converterinit();
            hfs_init_zones();
            hfs_sysctl_register();
            if ((2 * mp_ncpus) > MAX_CACHED_ORIGINS_DEFAULT) {
                _hfs_max_origins = 2 * mp_ncpus;
                _hfs_max_file_origins = 2 * mp_ncpus;
            } else if ((2 * mp_ncpus) > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
                _hfs_max_file_origins = 2 * mp_ncpus;
            }
            
            break;
            // is this right?
        case MOD_SHUTDOWN: /* FALLTHROUGH */
        case MOD_UNLOAD:
            uprintf("Unoading hfs kmod \n");
            hfs_destroy_zones();
            hfs_sysctl_unregister();
            hfs_converterdone();
            darwin_compat_uninit();
            break;
        default:
            returnCode = EOPNOTSUPP;
            break;
    }
    
    return(returnCode);
}


static moduledata_t  hfs_kmod_data = {
    "HFS",
    hfs_kmod_modevent,
    NULL
};

DECLARE_MODULE(hfs_kmod, hfs_kmod_data, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);


