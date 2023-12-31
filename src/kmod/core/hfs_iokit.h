/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef hfs_iokit_h
#define hfs_iokit_h

#include <sys/cdefs.h>
#include <sys/stdint.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/mount.h>

#if AppleKeyStoreFSServices
#include <AppleKeyStore/AppleKeyStoreFSServices.h>
#endif

__BEGIN_DECLS

bool hfs_is_ejectable(struct mount *mp, struct vnode *devvp);
void hfs_iterate_media_with_content(const char *content_uuid_cstring,
									int (*func)(const char *bsd_name,
												const char *uuid_str,
												void *arg),
									void *arg);
int hfs_get_platform_serial_number(char *serial_number_str,
											 uint32_t len);
#if AppleKeyStoreFSServices
int hfs_unwrap_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in,
				   aks_raw_key_t key_out);
int hfs_rewrap_key(aks_cred_t access, cp_key_class_t dp_class,
				   const aks_wrapped_key_t wrapped_key_in,
				   aks_wrapped_key_t wrapped_key_out);
int hfs_new_key(aks_cred_t access, cp_key_class_t dp_class,
				aks_raw_key_t key_out, aks_wrapped_key_t wrapped_key_out);
int hfs_backup_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in,
				   aks_wrapped_key_t wrapped_key_out);
#endif
__END_DECLS

#endif /* hfs_iokit_h */
