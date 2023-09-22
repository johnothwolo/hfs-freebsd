/*
 * Copyright (c) 2000-2015 Apple Inc. All rights reserved.
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
/*
 * Copyright (c) 1997-2002 Apple Inc. All Rights Reserved
 *
 */

#ifndef _HFS_MOUNT_H_
#define _HFS_MOUNT_H_

#include <sys/appleapiopts.h>

#include <sys/mount.h>
#include <sys/time.h>

#ifndef _KERNEL
#include <stdbool.h>
#endif

/*
 * Arguments to mount HFS-based filesystems
 */

#define OVERRIDE_UNKNOWN_PERMISSIONS 0

#define UNKNOWNUID ((uid_t)99)
#define UNKNOWNGID ((gid_t)99)
#define UNKNOWNPERMISSIONS (S_IRWXU | S_IROTH | S_IXOTH)		/* 705 */


struct hfs_mount_args {
	char	*fspec;			/* block special device to mount */
    struct  oexport_args export;    /* network export information */
	uid_t	            hfs_uid;		    /* uid that owns hfs files (standard HFS only) */
	gid_t	            hfs_gid;		    /* gid that owns hfs files (standard HFS only) */
	mode_t	            hfs_mask;		    /* mask to be applied for hfs perms  (standard HFS only) */
    
	u_int32_t           hfs_encoding;	    /* encoding for this volume (standard HFS only) */
	struct	timezone    hfs_timezone;	    /* user time zone info (standard HFS only) */
    bool                hfs_wrapper;        /* mount HFS wrapper (if it exists) */
    bool                hfs_noexec;         /* disable execute permissions for files */
    bool                hfs_unknown_perms;  /* support MNT_UNKNOWNPERMISSIONS */
	int     journal_tbuffer_size;   /* size in bytes of the journal transaction buffer */
	int		journal_flags;          /* flags to pass to journal_open/create */
	int		journal_disable;        /* don't use journaling (potentially dangerous) */
};

/*
 * Sysctl values for HFS
 */
#define HFS_ENCODINGBIAS	1	    /* encoding matching CJK bias */
#define HFS_EXTEND_FS		2
#define HFS_ENABLE_JOURNALING   0x082969
#define HFS_DISABLE_JOURNALING  0x031272
#define HFS_REPLAY_JOURNAL	    0x6a6e6c72
#define HFS_ENABLE_RESIZE_DEBUG 4	/* enable debug code for volume resizing */



#endif /* ! _HFS_MOUNT_H_ */
