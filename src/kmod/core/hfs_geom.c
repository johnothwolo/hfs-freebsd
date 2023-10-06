//
//  hfs_iokit.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/22/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/disk/gpt.h>
#include <sys/disk.h>

#include <sys/kobj.h>
#include <sys/linker_set.h>

#include <geom/geom.h>
#include <geom/geom_int.h>
#include <geom/geom_vfs.h>
#include <geom/part/g_part.h>

#include "hfs_iokit.h"

#define G_READONLY 0
#define GEOM_EJECTABLE_ATTTR "GEOM::?????" // FIXME: some attributes just aren't supported


// FiXME: should take cp from caller....
#if notyet
bool
hfs_is_ejectable(struct mount *mp, struct vnode *devvp, struct g_consumer *cp)
{
    int error, len;
    bool ejectable = false;
    
    if (error) {
        printf("WARNING: %s: Could not open geom disk (error %d)\n", mp->mnt_stat.f_mntfromname, error);
        // assume not removable
        goto out;
    }
    
    error = g_io_getattr(GEOM_EJECTABLE_ATTTR, cp, &len, &ejectable);
    
    if (error != 0) {
        panic("ERROR: %s: Could not get ejectable attribute for disk (error %d)\n", mp->mnt_stat.f_mntfromname, error);
    }
    
out:
    return ejectable;
}
#else
bool
hfs_is_ejectable(struct mount *mp, struct vnode *devvp)
{
	return true; // just return true
}
#endif

// FIXME: study GEOM architecture and figure this out
// MARK: disabled in open_journal_dev
void hfs_iterate_media_with_content(const char *content_uuid_cstring,
                                    int (*func)(const char *device,
                                                const char *uuid_str,
                                                void *arg),
                                    void *arg)
{
    // iterate through media and match uuid. 1 continues interation, 0 stops it.
    panic("Unimplemented");
}

int hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
    return -1;
}

#if AppleKeyStoreFSServices


// Interface with AKS

static aks_file_system_key_services_t *
key_services(void)
{
	static aks_file_system_key_services_t *g_key_services;

	if (!g_key_services) {
		IOService *platform = IOService::getPlatform();
		if (platform) {
			IOReturn ret = platform->callPlatformFunction
				(kAKSFileSystemKeyServices, true, &g_key_services, NULL, NULL, NULL);
			if (ret)
				printf("hfs: unable to get " kAKSFileSystemKeyServices " (0x%x)\n", ret);
		}
	}

	return g_key_services;
}

int hfs_unwrap_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in,
				   aks_raw_key_t key_out)
{
	aks_file_system_key_services_t *ks = key_services();
	if (!ks || !ks->unwrap_key)
		return ENXIO;
	return ks->unwrap_key(access, wrapped_key_in, key_out);
}

int hfs_rewrap_key(aks_cred_t access, cp_key_class_t dp_class,
				   const aks_wrapped_key_t wrapped_key_in,
				   aks_wrapped_key_t wrapped_key_out)
{
	aks_file_system_key_services_t *ks = key_services();
	if (!ks || !ks->rewrap_key)
		return ENXIO;
	return ks->rewrap_key(access, dp_class, wrapped_key_in, wrapped_key_out);
}

int hfs_new_key(aks_cred_t access, cp_key_class_t dp_class,
				aks_raw_key_t key_out, aks_wrapped_key_t wrapped_key_out)
{
	aks_file_system_key_services_t *ks = key_services();
	if (!ks || !ks->new_key)
		return ENXIO;
	return ks->new_key(access, dp_class, key_out, wrapped_key_out);
}

int hfs_backup_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in,
				   aks_wrapped_key_t wrapped_key_out)
{
	aks_file_system_key_services_t *ks = key_services();
	if (!ks || !ks->backup_key)
		return ENXIO;
	return ks->backup_key(access, wrapped_key_in, wrapped_key_out);
}

#endif
