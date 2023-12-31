/*
 * Copyright (c) 1999-2017 Apple Inc. All rights reserved.
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
 * Copyright (c) 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      hfs_vfsops.c
 *  derived from	@(#)ufs_vfsops.c	8.8 (Berkeley) 5/20/95
 *
 *      (c) Copyright 1997-2002 Apple Inc. All rights reserved.
 *
 *      hfs_vfsops.c -- VFS layer for loadable HFS file system.
 *
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/ucred.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/stat.h>
#include <sys/ddisk.h>
#include <sys/time.h>
#include <sys/endian.h>
#include <sys/namei.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/md5.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/kthread.h>

#include <sys/quota.h>
#include <sys/utfconv.h>

#include <geom/geom.h>
#include <geom/geom_int.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#include <uuid/uuid.h>
#include <kern/locks.h>
#include <sys/compat.h>

#include "hfs_journal.h"
#include "hfs_mount.h"


#include "hfs_iokit.h"
#include "hfs.h"
#include "hfs_catalog.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_endian.h"
#include "hfs_hotfiles.h"
#include "hfs_quota.h"
#include "hfs_btreeio.h"
#include "hfs_kdebug.h"
#include "hfs_cprotect.h"

#include "FileMgrInternal.h"
#include "BTreesInternal.h"

SDT_PROVIDER_DEFINE(hfs);

#define HFS_MOUNT_DEBUG 1

/* Enable/disable debugging code for live volume resizing, defined in hfs_resize.c */
extern int hfs_resize_debug;

lck_grp_attr_t *  hfs_group_attr;
lck_attr_t *  hfs_lock_attr;
lck_grp_t *  hfs_mutex_group;
lck_grp_t *  hfs_rwlock_group;
lck_grp_t *  hfs_spinlock_group;

// variables to manage HFS kext retain count -- only supported on Macs
#if	TARGET_OS_OSX
int hfs_active_mounts = 0;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;

#if CONFIG_HFS_STD
extern struct vnodeopv_desc hfs_std_vnodeop_opv_desc;
static int hfs_flushMDB(struct hfsmount *hfsmp, int waitfor, int altflush);
#endif

/* not static so we can re-use in hfs_readwrite.c for vn_getpath_ext calls */
vfs_vget_t hfs_vfs_vget;

static vfs_fhtovp_t hfs_fhtovp;
static vfs_init_t hfs_init;
static vfs_quotactl_t hfs_quotactl;
vfs_mount_t hfs_mount;
vfs_statfs_t hfs_statfs;
vfs_sync_t hfs_sync;
vfs_sysctl_t hfs_sysctl;
vfs_unmount_t hfs_unmount;

static int  hfs_changefs(struct mount *mp, struct hfs_mount_args *args);
static int  hfs_flushfiles(struct mount *, int, struct thread *);
static void hfs_locks_destroy(struct hfsmount *hfsmp);
static int  hfs_start(struct mount *mp, int flags, struct thread *td);
static void hfs_syncer_free(struct hfsmount *hfsmp);

void hfs_initialize_allocator (struct hfsmount *hfsmp);
int  hfs_teardown_allocator (struct hfsmount *hfsmp);

int hfs_mountfs(struct vnode *devvp, struct mount *mp, struct hfs_mount_args *args, int journal_replay_only, struct thread *td);
int hfs_reload(struct mount *mp, struct thread *td, int flags);

static int hfs_journal_replay(struct vnode* devvp, struct thread *td);

static int hfs_args_parse(struct mount*, struct hfs_mount_args *);

#if HFS_LEAK_DEBUG
#include <IOKit/IOLib.h>
#endif


static const char *hfs_opts[] = { "acls", "async", "noatime", "noclusterr",
    "noclusterw", "noexec", "export", "force", "from", "multilabel",
    "suiddir", "nosymfollow", "sync", "union", NULL };

/*
 * VFS Operations.
 *
 * mount system call
 */

int
hfs_mount(struct mount *mp)
{

#if HFS_LEAK_DEBUG

#warning HFS_LEAK_DEBUG is on

	hfs_alloc_trace_enable();

#endif
    struct thread *td;
    struct vnode *devvp;
    struct hfsmount *hfsmp;
	struct hfs_mount_args args;
    struct nameidata nd, *ndp = &nd;
    accmode_t accmode;
    struct vfsoptlist *opts;
    int retval;
	u_int32_t cmdflags;
    char *path, *fspec;
    int flags, len;
    
    flags = 0;
    retval = E_NONE;
    hfsmp = NULL;
    td = curthread;
    opts = mp->mnt_optnew;
    
    if (vfs_filteropt(opts, hfs_opts))
        trace_return (EINVAL);
    
    vfs_getopt(opts, "fspath", (void **)&path, NULL);
    /* Double-check the length of path.. */
    if (strlen(path) >= MAXMNTLEN)
        trace_return (ENAMETOOLONG);
    
    fspec = NULL;
    retval = vfs_getopt(opts, "from", (void **)&fspec, &len);
    if (!retval && fspec[len - 1] != '\0')
        trace_return (EINVAL);
    
    (void) hfs_args_parse(mp, &args);
    
	cmdflags = (u_int32_t)mp->mnt_flag & MNT_CMDFLAGS;
    
	if (cmdflags & MNT_UPDATE) {

		hfsmp = VFSTOHFS(mp);

		/* Reload incore data after an fsck. */
		if (cmdflags & MNT_RELOAD) {
			if ((mp->mnt_flag & MNT_RDONLY)) {
				int error = hfs_reload(mp, td, 0);
				if (error && HFS_MOUNT_DEBUG) {
					printf("hfs_mount: hfs_reload returned %d on %s \n", error, hfsmp->vcbVN);
				}
				return error;
			}
			else {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: MNT_RELOAD not supported on rdwr filesystem %s\n", hfsmp->vcbVN);
				}
				trace_return (EINVAL);
			}
		}

		/* Change to a read-only file system. */
		if (((hfsmp->hfs_flags & HFS_READ_ONLY) == 0) &&
		    (mp->mnt_flag & MNT_RDONLY)) {
			int flags;

			/* Set flag to indicate that a downgrade to read-only
			 * is in progress and therefore block any further 
			 * modifications to the file system.
			 */
			hfs_lock_global (hfsmp, HFS_EXCLUSIVE_LOCK);
			hfsmp->hfs_flags |= HFS_RDONLY_DOWNGRADE;
			hfsmp->hfs_downgrading_thread = curthread;
			hfs_unlock_global (hfsmp);
			hfs_syncer_free(hfsmp);
            
			/* use hfs_sync to push out System (btree) files */
			retval = hfs_sync(mp, MNT_WAIT);
			if (retval && ((cmdflags & MNT_FORCE) == 0)) {
				hfsmp->hfs_flags &= ~HFS_RDONLY_DOWNGRADE;
				hfsmp->hfs_downgrading_thread = NULL;
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: VFS_SYNC returned %d during b-tree sync of %s \n", retval, hfsmp->vcbVN);
				}
				goto out;
			}
		
			flags = WRITECLOSE;
			if (cmdflags & MNT_FORCE)
				flags |= FORCECLOSE;
				
			if ((retval = hfs_flushfiles(mp, flags, td))) {
				hfsmp->hfs_flags &= ~HFS_RDONLY_DOWNGRADE;
				hfsmp->hfs_downgrading_thread = NULL;
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: hfs_flushfiles returned %d on %s \n", retval, hfsmp->vcbVN);
				}
				goto out;
			}

			/* mark the volume cleanly unmounted */
			hfsmp->vcbAtrb |= kHFSVolumeUnmountedMask;
			retval = hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT);
			hfsmp->hfs_flags |= HFS_READ_ONLY;

			/*
			 * Close down the journal. 
			 *
			 * NOTE: It is critically important to close down the journal
			 * and have it issue all pending I/O prior to calling VNOP_FSYNC below.
			 * In a journaled environment it is expected that the journal be
			 * the only actor permitted to issue I/O for metadata blocks in HFS.
			 * If we were to call VNOP_FSYNC prior to closing down the journal,
			 * we would inadvertantly issue (and wait for) the I/O we just 
			 * initiated above as part of the flushvolumeheader call.
			 * 
			 * To avoid this, we follow the same order of operations as in
			 * unmount and issue the journal_close prior to calling VNOP_FSYNC.
			 */
	
			if (hfsmp->jnl) {
				hfs_lock_global (hfsmp, HFS_EXCLUSIVE_LOCK);

			    journal_close(hfsmp->jnl);
			    hfsmp->jnl = NULL;

			    // Note: we explicitly don't want to shutdown
			    //       access to the jvp because we may need
			    //       it later if we go back to being read-write.

				hfs_unlock_global (hfsmp);

                CLR(hfsmp->hfs_mp->mnt_flag, MNT_JOURNALED);
			}

			/*
			 * Write out any pending I/O still outstanding against the device node
			 * now that the journal has been closed.
			 */
			if (retval == 0) {
				vget(hfsmp->hfs_devvp, 0);
				retval = VOP_FSYNC(hfsmp->hfs_devvp, MNT_WAIT, td);
				vrele(hfsmp->hfs_devvp);
			}

			if (retval) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: FSYNC on devvp returned %d for fs %s\n", retval, hfsmp->vcbVN);
				}
				hfsmp->hfs_flags &= ~HFS_RDONLY_DOWNGRADE;
				hfsmp->hfs_downgrading_thread = NULL;
				hfsmp->hfs_flags &= ~HFS_READ_ONLY;
				goto out;
			}
		
			if (hfsmp->hfs_flags & HFS_SUMMARY_TABLE) {
				if (hfsmp->hfs_summary_table) {
					int err = 0;
					/* 
					 * Take the bitmap lock to serialize against a concurrent bitmap scan still in progress 
					 */
					if (hfsmp->hfs_allocation_vp) {
						err = hfs_lock (VTOC(hfsmp->hfs_allocation_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
					}
					hfs_free(hfsmp->hfs_summary_table, hfsmp->hfs_summary_bytes);
					hfsmp->hfs_summary_table = NULL;
					hfsmp->hfs_flags &= ~HFS_SUMMARY_TABLE;
					if (err == 0 && hfsmp->hfs_allocation_vp){
						hfs_unlock (VTOC(hfsmp->hfs_allocation_vp));
					}
				}
			}

			hfsmp->hfs_downgrading_thread = NULL;
		}

		/* Change to a writable file system. */
		if (mp->mnt_flag & MNT_RELOAD) {
			/*
			 * On inconsistent disks, do not allow read-write mount
			 * unless it is the boot volume being mounted.
			 */
			if (!(mp->mnt_flag & MNT_ROOTFS) &&
					(hfsmp->vcbAtrb & kHFSVolumeInconsistentMask)) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: attempting to mount inconsistent non-root volume %s\n",  (hfsmp->vcbVN));
				}
				retval = EINVAL;
				goto out;
			}

			// If the journal was shut-down previously because we were
			// asked to be read-only, let's start it back up again now
			
			if (   (HFSTOVCB(hfsmp)->vcbAtrb & kHFSVolumeJournaledMask)
			    && hfsmp->jnl == NULL
			    && hfsmp->jvp != NULL) {
			    int jflags;

			    if (hfsmp->hfs_flags & HFS_NEED_JNL_RESET) {
					jflags = JOURNAL_RESET;
				} else {
					jflags = 0;
				}

				hfs_lock_global (hfsmp, HFS_EXCLUSIVE_LOCK);

				/* We provide the mount point twice here: The first is used as
				 * an opaque argument to be passed back when hfs_sync_metadata
				 * is called.  The second is provided to the throttling code to
				 * indicate which mount's device should be used when accounting
				 * for metadata writes.
				 */
				hfsmp->jnl = journal_open(hfsmp->jvp,
                                          hfsmp->jcp,
						hfs_blk_to_bytes(hfsmp->jnl_start, HFSTOVCB(hfsmp)->blockSize) + (off_t)HFSTOVCB(hfsmp)->hfsPlusIOPosOffset,
						hfsmp->jnl_size,
						hfsmp->hfs_devvp,
						hfsmp->hfs_logical_block_size,
						jflags,
						0,
						hfs_sync_metadata, hfsmp->hfs_mp,
						hfsmp->hfs_mp);
				
				/*
				 * Set up the trim callback function so that we can add
				 * recently freed extents to the free extent cache once
				 * the transaction that freed them is written to the
				 * journal on disk.
				 */
				if (hfsmp->jnl)
                    journal_trim_set_callback(hfsmp->jnl, &hfs_trim_callback, hfsmp);
				
				hfs_unlock_global (hfsmp);

				if (hfsmp->jnl == NULL) {
					if (HFS_MOUNT_DEBUG) {
						printf("hfs_mount: journal_open == NULL; couldn't be opened on %s \n", (hfsmp->vcbVN));
					}
					retval = EINVAL;
					goto out;
				} else {
					hfsmp->hfs_flags &= ~HFS_NEED_JNL_RESET;
                    hfsmp->hfs_mp->mnt_flag |= MNT_JOURNALED;
				}
			}

			/* See if we need to erase unused Catalog nodes due to <rdar://problem/6947811>. */
			retval = hfs_erase_unused_nodes(hfsmp);
			if (retval != E_NONE) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: hfs_erase_unused_nodes returned %d for fs %s\n", retval, hfsmp->vcbVN);
				}
				goto out;
			}

			/* If this mount point was downgraded from read-write 
			 * to read-only, clear that information as we are now 
			 * moving back to read-write.
			 */
			hfsmp->hfs_flags &= ~HFS_RDONLY_DOWNGRADE;
			hfsmp->hfs_downgrading_thread = NULL;

			/* mark the volume dirty (clear clean unmount bit) */
			hfsmp->vcbAtrb &= ~kHFSVolumeUnmountedMask;

			retval = hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT);
			if (retval != E_NONE) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mount: hfs_flushvolumeheader returned %d for fs %s\n", retval, hfsmp->vcbVN);
				}
				goto out;
			}
		
			/* Only clear HFS_READ_ONLY after a successful write */
			hfsmp->hfs_flags &= ~HFS_READ_ONLY;


			if (!(hfsmp->hfs_flags & (HFS_READ_ONLY | HFS_STANDARD))) {
				/* Setup private/hidden directories for hardlinks. */
				hfs_privatedir_init(hfsmp, FILE_HARDLINKS);
				hfs_privatedir_init(hfsmp, DIR_HARDLINKS);

				hfs_remove_orphans(hfsmp);

				/*
				 * Since we're upgrading to a read-write mount, allow
				 * hot file clustering if conditions allow.
				 *
				 * Note: this normally only would happen if you booted
				 *       single-user and upgraded the mount to read-write
				 *
				 * Note: at this point we are not allowed to fail the
				 *       mount operation because the HotFile init code
				 *       in hfs_recording_init() will lookup vnodes with
				 *       VNOP_LOOKUP() which hangs vnodes off the mount
				 *       (and if we were to fail, VFS is not prepared to
				 *       clean that up at this point.  Since HotFiles are
				 *       optional, this is not a big deal.
				 */
				if (ISSET(hfsmp->hfs_flags, HFS_METADATA_ZONE)
					&& (!ISSET(hfsmp->hfs_flags, HFS_SSD)
						|| ISSET(hfsmp->hfs_flags, HFS_CS_HOTFILE_PIN))) {
					hfs_recording_init(hfsmp);
				}					
				/* Force ACLs on HFS+ file systems. */
				if (((HFSTOVFS(hfsmp))->mnt_flag & MNT_ACLS) == 0) {
                    (HFSTOVFS(hfsmp))->mnt_flag |= MNT_ACLS;
				}
			}
		}

		/* Update file system parameters. */
		retval = hfs_changefs(mp, &args);
		if (retval &&  HFS_MOUNT_DEBUG) {
			printf("hfs_mount: hfs_changefs returned %d for %s\n", retval, hfsmp->vcbVN);
		}

	} else /* not an update request */ {
        /*
         * Not an update, or updating the name: look up the name
         * and verify that it refers to a sensible disk device.
         */
        if (fspec == NULL)
            trace_return (EINVAL);
        NDINIT(ndp, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, fspec, td);
        if ((retval = namei(ndp)) != 0)
            trace_return (retval);
        NDFREE(ndp, NDF_ONLY_PNBUF);
        devvp = ndp->ni_vp;

        if (!vn_isdisk_error(devvp, &retval)) {
            vput(devvp);
            trace_return (retval);
        }
        
        if (devvp == NULL) {
            retval = EINVAL;
            goto out;
        }
        
        /*
         * If mount by non-root, then verify that user has necessary
         * permissions on the device.
         *
         * XXXRW: VOP_ACCESS() enough?
         */
        accmode = VREAD;
        // FIXME: write support
//        if ((mp->mnt_flag & MNT_RDONLY) == 0)
//            accmode |= VWRITE;
        retval = VOP_ACCESS(devvp, accmode, td->td_ucred, td);
        if (retval)
            retval = priv_check(td, PRIV_VFS_MOUNT_PERM);
        if (retval) {
            vput(devvp);
            trace_return (retval);
        }
        
		SET(mp->mnt_flag, (u_int64_t)((unsigned int)MNT_RDONLY));

		retval = hfs_mountfs(devvp, mp, NULL, 0, td);
		if (retval) { 
			const char *name = devtoname(devvp->v_rdev);
			printf("hfs_mount: hfs_mountfs returned error=%d for device %s\n", retval, (name ? name : "unknown-dev"));
            vrele(devvp);
			goto out;
		}

		/* After hfs_mountfs succeeds, we should have valid hfsmp */
		hfsmp = VFSTOHFS(mp);

		/* Set up the maximum defrag file size */
		hfsmp->hfs_defrag_max = HFS_INITIAL_DEFRAG_SIZE;

        // FIXME: permissions
//		if (!data) {
			// Root mount

			hfsmp->hfs_uid = UNKNOWNUID;
			hfsmp->hfs_gid = UNKNOWNGID;
			hfsmp->hfs_dir_mask = (S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH); /* 0755 */
			hfsmp->hfs_file_mask = (S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH); /* 0755 */

			/* Establish the free block reserve. */
			hfsmp->reserveBlocks = (uint32_t) ((u_int64_t)hfsmp->totalBlocks * HFS_MINFREE) / 100;
			hfsmp->reserveBlocks = MIN(hfsmp->reserveBlocks, HFS_MAXRESERVE / hfsmp->blockSize);
//		}
#if	TARGET_OS_OSX 
		// increment kext retain count
		atomic_add_int((volatile u_int *)&hfs_active_mounts, 1);
//		OSKextRetainKextWithLoadTag(OSKextGetCurrentLoadTag());
		if (hfs_active_mounts <= 0 && panic_on_assert)
			panic("hfs_mount: error - kext resource count is non-positive: %d but at least one active mount\n", hfs_active_mounts);
#endif
	}
    
    vfs_mountedfrom(mp, fspec);
    
out:
	if (retval == 0) {
		(void)hfs_statfs(mp, &(mp)->mnt_stat);
	}
	trace_return (retval);
}

/* Change fs mount parameters */
static int
hfs_changefs(struct mount *mp, struct hfs_mount_args *args)
{
	int retval = 0;
    struct vnode *vp, *mvp;
	int namefix, permfix, permswitch;
	struct hfsmount *hfsmp;
	ExtendedVCB *vcb;
	u_int32_t mount_flags;

#if CONFIG_HFS_STD
	u_int32_t old_encoding = 0;
	hfs_to_unicode_func_t	get_unicode_func;
	unicode_to_hfs_func_t	get_hfsname_func = NULL;
#endif

	hfsmp = VFSTOHFS(mp);
	vcb = HFSTOVCB(hfsmp);
	mount_flags = (unsigned int)mp->mnt_flag;

	hfsmp->hfs_flags |= HFS_IN_CHANGEFS;
	
	permswitch = (((hfsmp->hfs_flags & HFS_UNKNOWN_PERMS) && !args->hfs_unknown_perms) ||
	              (((hfsmp->hfs_flags & HFS_UNKNOWN_PERMS) == 0) && args->hfs_unknown_perms));

#if rootmount
	/* The root filesystem must operate with actual permissions: */
	if (permswitch && (mount_flags & MNT_ROOTFS) && args->hfs_unknown_perms) {
		args->hfs_unknown_perms = false;	/* Just say "No". */
		retval = EINVAL;
		goto exit;
	}
#endif
    
	if (args->hfs_unknown_perms)
		hfsmp->hfs_flags |= HFS_UNKNOWN_PERMS;
	else
		hfsmp->hfs_flags &= ~HFS_UNKNOWN_PERMS;

	namefix = permfix = 0;

	/*
	 * Tracking of hot files requires up-to-date access times.  So if
	 * access time updates are disabled, we must also disable hot files.
	 */
	if (mount_flags & MNT_NOATIME) {
		(void) hfs_recording_suspend(hfsmp);
	}
	
	/* Change the timezone (Note: this affects all hfs volumes and hfs+ volume create dates) */
	if (args->hfs_timezone.tz_minuteswest != VNOVAL) {
		gTimeZone = args->hfs_timezone;
	}

	/* Change the default uid, gid and/or mask */
	if ((args->hfs_uid != (uid_t)VNOVAL) && (hfsmp->hfs_uid != args->hfs_uid)) {
		hfsmp->hfs_uid = args->hfs_uid;
		if (vcb->vcbSigWord == kHFSPlusSigWord)
			++permfix;
	}
	if ((args->hfs_gid != (gid_t)VNOVAL) && (hfsmp->hfs_gid != args->hfs_gid)) {
		hfsmp->hfs_gid = args->hfs_gid;
		if (vcb->vcbSigWord == kHFSPlusSigWord)
			++permfix;
	}
	if (args->hfs_mask != (mode_t)VNOVAL) {
		if (hfsmp->hfs_dir_mask != (args->hfs_mask & ALLPERMS)) {
			hfsmp->hfs_dir_mask = args->hfs_mask & ALLPERMS;
			hfsmp->hfs_file_mask = args->hfs_mask & ALLPERMS;
			if (args->hfs_noexec)
				hfsmp->hfs_file_mask = (args->hfs_mask & DEFFILEMODE);
			if (vcb->vcbSigWord == kHFSPlusSigWord)
				++permfix;
		}
	}
	
#if CONFIG_HFS_STD
	/* Change the hfs encoding value (hfs only) */
	if ((vcb->vcbSigWord == kHFSSigWord)	&&
	    (args->hfs_encoding != (u_int32_t)VNOVAL)              &&
	    (hfsmp->hfs_encoding != args->hfs_encoding)) {

		retval = hfs_getconverter(args->hfs_encoding, &get_unicode_func, &get_hfsname_func);
		if (retval)
			goto exit;

		/*
		 * Connect the new hfs_get_unicode converter but leave
		 * the old hfs_get_hfsname converter in place so that
		 * we can lookup existing vnodes to get their correctly
		 * encoded names.
		 *
		 * When we're all finished, we can then connect the new
		 * hfs_get_hfsname converter and release our interest
		 * in the old converters.
		 */
		hfsmp->hfs_get_unicode = get_unicode_func;
		old_encoding = hfsmp->hfs_encoding;
		hfsmp->hfs_encoding = args->hfs_encoding;
		++namefix;
	}
#endif

	if (!(namefix || permfix || permswitch))
		goto exit;

	/* XXX 3762912 hack to support HFS filesystem 'owner' */
//	if (permfix) {
//		vfs_setowner(mp,
//		    hfsmp->hfs_uid == UNKNOWNUID ? KAUTH_UID_NONE : hfsmp->hfs_uid,
//		    hfsmp->hfs_gid == UNKNOWNGID ? KAUTH_GID_NONE : hfsmp->hfs_gid);
//	}

	/*
	 * For each active vnode fix things that changed
	 *
	 * Note that we can visit a vnode more than once
	 * and we can race with fsync.
	 *
	 * hfs_changefs_callback will be called for each vnode
	 * hung off of this mount point
	 *
	 * The vnode will be properly referenced and unreferenced 
	 * around the callback
	 */

loop:
    MNT_VNODE_FOREACH_ALL(vp, mp, mvp) {
        ExtendedVCB *vcb;
        struct cnode *cp;
        struct cat_desc cndesc;
        struct cat_attr cnattr;
        int lockflags;
        int error;
        
        cp = VTOC(vp);
        vcb = HFSTOVCB(hfsmp);
        
        // acquire vnode lock
        if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK)) {
            MNT_VNODE_FOREACH_ALL_ABORT(mp, mvp);
            goto loop;
        }
        
        // lock catalog file
        lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
        error = cat_lookup(hfsmp, &cp->c_desc, 0, 0, &cndesc, &cnattr, NULL, NULL);
        hfs_systemfile_unlock(hfsmp, lockflags);
        if (error) {
            /*
             * If we couldn't find this guy skip to the next one
             */
            if (namefix){
                cache_purge(vp);
            }

            vrele(vp);
            VOP_UNLOCK(vp);
            continue;
        }
        /*
         * Get the real uid/gid and perm mask from disk.
         */
        if (permswitch || permfix) {
            cp->c_uid = cnattr.ca_uid;
            cp->c_gid = cnattr.ca_gid;
            cp->c_mode = cnattr.ca_mode;
        }
        /*
         * If we're switching name converters then...
         *   Remove the existing entry from the namei cache.
         *   Update name to one based on new encoder.
         */
        if (namefix) {
            cache_purge(vp);
            replace_desc(cp, &cndesc);
            
            if (cndesc.cd_cnid == kHFSRootFolderID) {
                strlcpy((char *)vcb->vcbVN, (const char *)cp->c_desc.cd_nameptr, NAME_MAX+1);
                cp->c_desc.cd_encoding = hfsmp->hfs_encoding;
            }
        } else {
            cat_releasedesc(&cndesc);
        }
    }

#if CONFIG_HFS_STD
	/*
	 * If we're switching name converters we can now
	 * connect the new hfs_get_hfsname converter and
	 * release our interest in the old converters.
	 */
	if (namefix) {
		/* HFS standard only */
		hfsmp->hfs_get_hfsname = get_hfsname_func;
		vcb->volumeNameEncodingHint = args->hfs_encoding;
		(void) hfs_relconverter(old_encoding);
	}
#endif

exit:
	hfsmp->hfs_flags &= ~HFS_IN_CHANGEFS;
	trace_return (retval);
}

/*
 * Reload all incore data for a filesystem (used after running fsck on
 * the root filesystem and finding things to fix). The filesystem must
 * be mounted read-only.
 *
 * Things to do to update the mount:
 *	invalidate all cached meta-data.
 *	invalidate all inactive vnodes.
 *	invalidate all cached file data.
 *	re-read volume header from disk.
 *	re-load meta-file info (extents, file size).
 *	re-load B-tree header data.
 *	re-read cnode data for all active vnodes.
 */
int
hfs_reload(struct mount *mountp, struct thread *td, int flags)
{
    struct vnode *vp, *mvp, *devvp;
	struct buf *bp;
	int error, i;
	struct hfsmount *hfsmp;
	struct HFSPlusVolumeHeader *vhp;
	ExtendedVCB *vcb;
	struct filefork *forkp;
    	struct cat_desc cndesc;
	daddr_t priIDSector;

    error = 0;
    hfsmp = VFSTOHFS(mountp);
	vcb = HFSTOVCB(hfsmp);

	if (vcb->vcbSigWord == kHFSSigWord)
		trace_return (EINVAL);	/* rooting from HFS is not supported! */

	/*
	 * Invalidate all cached meta-data.
	 */
	devvp = hfsmp->hfs_devvp;
	if (vinvalbuf(devvp, 0, 0, 0))
		panic("hfs_reload: dirty1");

	/*
	 * hfs_reload_callback will be called for each vnode
	 * hung off of this mount point that can't be recycled...
	 * vnode_iterate will recycle those that it can (the VNODE_RELOAD option)
	 * the vnode will be in an 'unbusy' state (VNODE_WAIT) and 
	 * properly referenced and unreferenced around the callback
	 */
loop:
    MNT_VNODE_FOREACH_ALL(vp, mountp, mvp) {
        struct cnode *cp;
        int lockflags;
        
        
        if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK)) {
            MNT_VNODE_FOREACH_ALL_ABORT(mountp, mvp);
            goto loop;
        }
        
        /*
         * flush all the buffers associated with this node
         */
        (void) vinvalbuf(vp, 0, 0, 0);
        
        cp = VTOC(vp);
        /*
         * Remove any directory hints
         */
        if ((vp->v_type & VDIR))
            hfs_reldirhints(cp, 0);
        
        /*
         * Re-read cnode data for all active vnodes (non-metadata files).
         */
        if ((vp->v_vflag & VV_SYSTEM) == 0 && !VNODE_IS_RSRC(vp) && (cp->c_fileid >= kHFSFirstUserCatalogNodeID)) {
            struct cat_fork *datafork;
            struct cat_desc desc;
            
            datafork = cp->c_datafork ? &cp->c_datafork->ff_data : NULL;
            
            /* lookup by fileID since name could have changed */
            lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
            error = cat_idlookup(hfsmp, cp->c_fileid, 0, 0, &desc, &cp->c_attr, datafork);
            hfs_systemfile_unlock(hfsmp, lockflags);
            if (error) {
                VOP_UNLOCK(vp);
                vrele(vp);
                MNT_VNODE_FOREACH_ALL_ABORT(mountp, mvp);
                return error;
            }
            
            /* update cnode's catalog descriptor */
            (void) replace_desc(cp, &desc);
        }
    }
    
	/*
	 * Re-read VolumeHeader from disk.
	 */
	priIDSector = (daddr_t)((vcb->hfsPlusIOPosOffset / hfsmp->hfs_logical_block_size) + 
			HFS_PRI_SECTOR(hfsmp->hfs_logical_block_size));

	error = (int)bread(hfsmp->hfs_devvp, HFS_PHYSBLK_ROUNDDOWN(priIDSector, hfsmp->hfs_log_per_phys),
                       hfsmp->hfs_physical_block_size, NOCRED, &bp);
	if (error) {
        	if (bp != NULL)
        		brelse(bp);
		trace_return (error);
	}

	vhp = (HFSPlusVolumeHeader *) (bp->b_data + HFS_PRI_OFFSET(hfsmp->hfs_physical_block_size));

	/* Do a quick sanity check */
	if ((SWAP_BE16(vhp->signature) != kHFSPlusSigWord &&
	     SWAP_BE16(vhp->signature) != kHFSXSigWord) ||
	    (SWAP_BE16(vhp->version) != kHFSPlusVersion &&
	     SWAP_BE16(vhp->version) != kHFSXVersion) ||
	    SWAP_BE32(vhp->blockSize) != vcb->blockSize) {
		brelse(bp);
		trace_return (EIO);
	}

	vcb->vcbLsMod		= to_bsd_time(SWAP_BE32(vhp->modifyDate));
	vcb->vcbAtrb		= SWAP_BE32 (vhp->attributes);
	vcb->vcbJinfoBlock  = SWAP_BE32(vhp->journalInfoBlock);
	vcb->vcbClpSiz		= SWAP_BE32 (vhp->rsrcClumpSize);
	vcb->vcbNxtCNID		= SWAP_BE32 (vhp->nextCatalogID);
	vcb->vcbVolBkUp		= to_bsd_time(SWAP_BE32(vhp->backupDate));
	vcb->vcbWrCnt		= SWAP_BE32 (vhp->writeCount);
	vcb->vcbFilCnt		= SWAP_BE32 (vhp->fileCount);
	vcb->vcbDirCnt		= SWAP_BE32 (vhp->folderCount);
	HFS_UPDATE_NEXT_ALLOCATION(vcb, SWAP_BE32 (vhp->nextAllocation));
	vcb->totalBlocks	= SWAP_BE32 (vhp->totalBlocks);
	vcb->freeBlocks		= SWAP_BE32 (vhp->freeBlocks);
	vcb->encodingsBitmap	= SWAP_BE64 (vhp->encodingsBitmap);
	bcopy(vhp->finderInfo, vcb->vcbFndrInfo, sizeof(vhp->finderInfo));    
	vcb->localCreateDate	= SWAP_BE32 (vhp->createDate); /* hfs+ create date is in local time */ 

	/*
	 * Re-load meta-file vnode data (extent info, file size, etc).
	 */
	forkp = VTOF((struct vnode *)vcb->extentsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		forkp->ff_extents[i].startBlock =
			SWAP_BE32 (vhp->extentsFile.extents[i].startBlock);
		forkp->ff_extents[i].blockCount =
			SWAP_BE32 (vhp->extentsFile.extents[i].blockCount);
	}
	forkp->ff_size      = SWAP_BE64 (vhp->extentsFile.logicalSize);
	forkp->ff_blocks    = SWAP_BE32 (vhp->extentsFile.totalBlocks);
	forkp->ff_clumpsize = SWAP_BE32 (vhp->extentsFile.clumpSize);


	forkp = VTOF((struct vnode *)vcb->catalogRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		forkp->ff_extents[i].startBlock	=
			SWAP_BE32 (vhp->catalogFile.extents[i].startBlock);
		forkp->ff_extents[i].blockCount	=
			SWAP_BE32 (vhp->catalogFile.extents[i].blockCount);
	}
	forkp->ff_size      = SWAP_BE64 (vhp->catalogFile.logicalSize);
	forkp->ff_blocks    = SWAP_BE32 (vhp->catalogFile.totalBlocks);
	forkp->ff_clumpsize = SWAP_BE32 (vhp->catalogFile.clumpSize);

	if (hfsmp->hfs_attribute_vp) {
		forkp = VTOF(hfsmp->hfs_attribute_vp);
		for (i = 0; i < kHFSPlusExtentDensity; i++) {
			forkp->ff_extents[i].startBlock	=
				SWAP_BE32 (vhp->attributesFile.extents[i].startBlock);
			forkp->ff_extents[i].blockCount	=
				SWAP_BE32 (vhp->attributesFile.extents[i].blockCount);
		}
		forkp->ff_size      = SWAP_BE64 (vhp->attributesFile.logicalSize);
		forkp->ff_blocks    = SWAP_BE32 (vhp->attributesFile.totalBlocks);
		forkp->ff_clumpsize = SWAP_BE32 (vhp->attributesFile.clumpSize);
	}

	forkp = VTOF((struct vnode *)vcb->allocationsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		forkp->ff_extents[i].startBlock	=
			SWAP_BE32 (vhp->allocationFile.extents[i].startBlock);
		forkp->ff_extents[i].blockCount	=
			SWAP_BE32 (vhp->allocationFile.extents[i].blockCount);
	}
	forkp->ff_size      = SWAP_BE64 (vhp->allocationFile.logicalSize);
	forkp->ff_blocks    = SWAP_BE32 (vhp->allocationFile.totalBlocks);
	forkp->ff_clumpsize = SWAP_BE32 (vhp->allocationFile.clumpSize);

	brelse(bp);
	vhp = NULL;

	/*
	 * Re-load B-tree header data
	 */
	forkp = VTOF((struct vnode *)vcb->extentsRefNum);
	if ( (error = MacToVFSError( BTReloadData((FCB*)forkp) )) )
		trace_return (error);

	forkp = VTOF((struct vnode *)vcb->catalogRefNum);
	if ( (error = MacToVFSError( BTReloadData((FCB*)forkp) )) )
		trace_return (error);

	if (hfsmp->hfs_attribute_vp) {
		forkp = VTOF(hfsmp->hfs_attribute_vp);
		if ( (error = MacToVFSError( BTReloadData((FCB*)forkp) )) )
			trace_return (error);
	}

	/* Reload the volume name */
	if ((error = cat_idlookup(hfsmp, kHFSRootFolderID, 0, 0, &cndesc, NULL, NULL)))
		trace_return (error);
	vcb->volumeNameEncodingHint = cndesc.cd_encoding;
	bcopy(cndesc.cd_nameptr, vcb->vcbVN, min(255, cndesc.cd_namelen));
	cat_releasedesc(&cndesc);

	/* Re-establish private/hidden directories. */
	hfs_privatedir_init(hfsmp, FILE_HARDLINKS);
	hfs_privatedir_init(hfsmp, DIR_HARDLINKS);

	/* In case any volume information changed to trigger a notification */
	hfs_generate_volume_notifications(hfsmp);
    
	return (0);
}

__unused
static uint64_t tv_to_usecs(struct timeval *tv)
{
	return tv->tv_sec * 1000000ULL + tv->tv_usec;
}

// Returns TRUE if b - a >= usecs
static bool hfs_has_elapsed (time_t a,
                             time_t b,
                             uint64_t usecs)
{
    
    return (TICKS_2_USEC(b) - TICKS_2_USEC(a)) >= usecs;
}
	
void hfs_syncer(void *arg)
{
    struct hfsmount *hfsmp = arg;

	KDBG(HFSDBG_SYNCER | DBG_FUNC_START, obfuscate_addr(hfsmp));

    hfs_syncer_lock(hfsmp);

	while (ISSET(hfsmp->hfs_flags, HFS_RUN_SYNCER)
		   && hfsmp->hfs_sync_req_oldest) {
        
		hfs_syncer_wait(hfsmp, &HFS_META_DELAY_TS);

		if (!ISSET(hfsmp->hfs_flags, HFS_RUN_SYNCER)
			|| !hfsmp->hfs_sync_req_oldest) {
			break;
		}

		/*
         *  Check to see whether we should flush now:
         *      HFS_META_DELAY has elapsed since
         *      the request and there are no pending writes.
         *      We won't measure the idle time here.
         */

		if (!hfs_has_elapsed(hfsmp->hfs_sync_req_oldest, ticks, HFS_MAX_META_DELAY)) {
			continue;
		}

        hfsmp->hfs_sync_req_oldest = 0;

		hfs_syncer_unlock(hfsmp);

		KDBG(HFSDBG_SYNCER_TIMED | DBG_FUNC_START, obfuscate_addr(hfsmp));

		/*
		 * We intentionally do a synchronous flush (of the journal or entire volume) here.
		 * For journaled volumes, this means we wait until the metadata blocks are written
		 * to both the journal and their final locations (in the B-trees, etc.).
		 *
		 * This tends to avoid interleaving the metadata writes with other writes (for
		 * example, user data, or to the journal when a later transaction notices that
		 * an earlier transaction has finished its async writes, and then updates the
		 * journal start in the journal header).  Avoiding interleaving of writes is
		 * very good for performance on simple flash devices like SD cards, thumb drives;
		 * and on devices like floppies.  Since removable devices tend to be this kind of
		 * simple device, doing a synchronous flush actually improves performance in
		 * practice.
		 *
		 * NOTE: For non-journaled volumes, the call to hfs_sync will also cause dirty
		 * user data to be written.
		 */
		if (hfsmp->jnl) {
			hfs_flush(hfsmp, HFS_FLUSH_JOURNAL_META);
		} else {
			hfs_sync(hfsmp->hfs_mp, MNT_WAIT);
		}

		KDBG(HFSDBG_SYNCER_TIMED | DBG_FUNC_END);

		hfs_syncer_lock(hfsmp);
	} // while (...)

	hfsmp->hfs_syncer_thread = NULL;
	hfs_syncer_unlock(hfsmp);
	hfs_syncer_wakeup(hfsmp);
    kthread_exit();

    /* BE CAREFUL WHAT YOU ADD HERE: at this point hfs_unmount is free
       to continue and therefore hfsmp might be invalid. */
    
    KDBG(HFSDBG_SYNCER | DBG_FUNC_END);
}

/*
 * Call into the allocator code and perform a full scan of the bitmap file.
 * 
 * This allows us to TRIM unallocated ranges if needed, and also to build up
 * an in-memory summary table of the state of the allocated blocks.
 */
void hfs_scan_blocks(void *arg)
{
    struct hfsmount *hfsmp = arg;
	/*
	 * Take the allocation file lock.  Journal transactions will block until
	 * we're done here. 
	 */
	
	int flags = hfs_systemfile_lock(hfsmp, SFL_BITMAP, HFS_EXCLUSIVE_LOCK);
	
	/* 
	 * We serialize here with the HFS mount lock as we're mounting.
	 * 
	 * The mount can only proceed once this thread has acquired the bitmap 
	 * lock, since we absolutely do not want someone else racing in and 
	 * getting the bitmap lock, doing a read/write of the bitmap file, 
	 * then us getting the bitmap lock.
	 * 
	 * To prevent this, the mount thread takes the HFS mount mutex, starts us 
	 * up, then immediately msleeps on the scan_var variable in the mount 
	 * point as a condition variable.  This serialization is safe since 
	 * if we race in and try to proceed while they're still holding the lock, 
	 * we'll block trying to acquire the global lock.  Since the mount thread 
	 * acquires the HFS mutex before starting this function in a new thread, 
	 * any lock acquisition on our part must be linearizably AFTER the mount thread's. 
	 *
	 * Note that the HFS mount mutex is always taken last, and always for only
	 * a short time.  In this case, we just take it long enough to mark the
	 * scan-in-flight bit.
	 */
	(void) hfs_lock_mount (hfsmp);
	hfsmp->scan_var |= HFS_ALLOCATOR_SCAN_INFLIGHT;
	wakeup((caddr_t) &hfsmp->scan_var);
	hfs_unlock_mount (hfsmp);

	/* Initialize the summary table */
	if (hfs_init_summary (hfsmp)) {
		printf("hfs: could not initialize summary table for %s\n", hfsmp->vcbVN);
	}	

	/*
	 * ScanUnmapBlocks assumes that the bitmap lock is held when you 
	 * call the function. We don't care if there were any errors issuing unmaps.
	 *
	 * It will also attempt to build up the summary table for subsequent
	 * allocator use, as configured.
	 */
	(void) ScanUnmapBlocks(hfsmp);

	(void) hfs_lock_mount (hfsmp);
	hfsmp->scan_var &= ~HFS_ALLOCATOR_SCAN_INFLIGHT;
	hfsmp->scan_var |= HFS_ALLOCATOR_SCAN_COMPLETED;
	wakeup((caddr_t) &hfsmp->scan_var);
	hfs_unlock_mount (hfsmp);

	vinvalbuf(hfsmp->hfs_allocation_vp, 0, 0, 0);
	
	hfs_systemfile_unlock(hfsmp, flags);
    kthread_exit();
}

/*
 * Common code for mount and mountroot
 */
int
hfs_mountfs(struct vnode *devvp, struct mount *mp, struct hfs_mount_args *args,
            int journal_replay_only, struct thread *td)
{
	struct proc *p = td->td_proc;
	int retval = E_NONE;
	struct hfsmount	*hfsmp = NULL;
	struct g_consumer *cp = NULL;
	struct buf *bp;
	struct bufobj *bo;
	struct cdev* dev;
	HFSMasterDirectoryBlock *mdbp = NULL;
	int ronly;
#if QUOTA
	int i;
#endif
	int mntwrapper;
	struct ucred* cred;
	u_int64_t disksize;
	daddr_t log_blkcnt;
	u_int32_t log_blksize;
	u_int32_t phys_blksize;
	u_int32_t minblksize;
	u_int32_t __unused iswritable;
	daddr_t mdb_offset;
	int __unused isvirtual = 0;
#if rootmount
	int isroot = !journal_replay_only && args == NULL;
#endif
	u_int32_t __unused device_features = 0;
	int __unused isssd;

	ronly = mp && (mp->mnt_flag & MNT_RDONLY);
	dev = (devvp->v_rdev);
	cred = p ? td->td_ucred : NOCRED;
	mntwrapper = 0;

	bp = NULL;
	hfsmp = NULL;
	mdbp = NULL;
	minblksize = kHFSBlockSize;

	/* Advisory locking should be handled at the VFS layer */
//    if (mp) {
//        vfs_setlocklocal(mp);
//    }

	g_topology_lock();
    retval = g_vfs_open(devvp, &cp, "hfs", ronly ? 0 : 1);
    g_topology_unlock();
    VOP_UNLOCK(devvp);
    
	if (retval != 0){
        if (cp) {
            g_topology_lock();
            g_vfs_close(cp);
            g_topology_unlock();
        }
		return retval;
	}
	bo = &devvp->v_bufobj;
	bo->bo_private = cp;
	bo->bo_ops = g_vfs_bufops;

	/* Get the logical block size (treated as physical block size everywhere) */
	if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKSIZE, (caddr_t)&log_blksize, 0)) {
		if (HFS_MOUNT_DEBUG) {
			printf("hfs_mountfs: DKIOCGETBLOCKSIZE failed\n");
		}
		retval = ENXIO;
		goto error_exit;
	}

	if (log_blksize == 0 || log_blksize > 1024*1024*1024) {
		printf("hfs: logical block size 0x%x looks bad.  Not mounting.\n", log_blksize);
		retval = ENXIO;
		goto error_exit;
	}
	
	/* Get the physical block size. */
	retval = VNOP_IOCTL(devvp, cp, DKIOCGETPHYSICALBLOCKSIZE, (caddr_t)&phys_blksize, 0);
	if (retval) {
		if ((retval != ENOTSUP) && (retval != ENOTTY)) {
			if (HFS_MOUNT_DEBUG) {
				printf("hfs_mountfs: DKIOCGETPHYSICALBLOCKSIZE failed\n");
			}
			retval = ENXIO;
			goto error_exit;
		}
		/* If device does not support this ioctl, assume that physical 
		 * block size is same as logical block size 
		 */
		phys_blksize = log_blksize;
	}
	if (phys_blksize == 0 || phys_blksize > MAXBSIZE) {
		printf("hfs: physical block size 0x%x looks bad.  Not mounting.\n", phys_blksize);
		retval = ENXIO;
		goto error_exit;
	}

	if (phys_blksize < log_blksize) {
		/*
		 * In the off chance that the phys_blksize is SMALLER than the logical
		 * then don't let that happen.  Pretend that the PHYSICALBLOCKSIZE
		 * ioctl was not supported.
		 */
		phys_blksize = log_blksize;
	}


	/* Switch to 512 byte sectors (temporarily) */
	if (log_blksize > 512) {
		u_int32_t size512 = 512;

		if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKSIZE, (caddr_t)&size512, 0)) {
			if (HFS_MOUNT_DEBUG) {
				printf("hfs_mountfs: DKIOCSETBLOCKSIZE failed \n");
			}
			retval = ENXIO;
			goto error_exit;
		}
	}
	/* Get the number of 512 byte physical blocks. */
	if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKCOUNT, (caddr_t)&log_blkcnt, 0)) {
		/* resetting block size may fail if getting block count did */
		(void)VNOP_IOCTL(devvp, cp, DKIOCSETBLOCKSIZE, (caddr_t)&log_blksize, 0);
		if (HFS_MOUNT_DEBUG) {
			printf("hfs_mountfs: DKIOCGETBLOCKCOUNT failed\n");
		}
		retval = ENXIO;
		goto error_exit;
	}
	/* Compute an accurate disk size (i.e. within 512 bytes) */
	disksize = (u_int64_t)log_blkcnt * (u_int64_t)512;

	/*
	 * On Tiger it is not necessary to switch the device 
	 * block size to be 4k if there are more than 31-bits
	 * worth of blocks but to insure compatibility with
	 * pre-Tiger systems we have to do it.
	 *
	 * If the device size is not a multiple of 4K (8 * 512), then
	 * switching the logical block size isn't going to help because
	 * we will be unable to write the alternate volume header.
	 * In this case, just leave the logical block size unchanged.
	 */
	if (log_blkcnt > 0x000000007fffffff && (log_blkcnt & 7) == 0) {
		minblksize = log_blksize = 4096;
		if (phys_blksize < log_blksize)
			phys_blksize = log_blksize;
	}
	
	/*
	 * The cluster layer is not currently prepared to deal with a logical
	 * block size larger than the system's page size.  (It can handle 
	 * blocks per page, but not multiple pages per block.)  So limit the
	 * logical block size to the page size.
	 */
	if (log_blksize > PAGE_SIZE) {
		log_blksize = PAGE_SIZE;
	}

	/* Now switch to our preferred physical block size. */
	if (log_blksize > 512) {
        if (VNOP_IOCTL(devvp, cp, DKIOCSETBLOCKSIZE, (caddr_t)&log_blksize, 0)) {
			if (HFS_MOUNT_DEBUG) { 
				printf("hfs_mountfs: DKIOCSETBLOCKSIZE (2) failed\n");
			}
			retval = ENXIO;
			goto error_exit;
		}
		/* Get the count of physical blocks. */
		if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKCOUNT, (caddr_t)&log_blkcnt, 0)) {
			if (HFS_MOUNT_DEBUG) { 
				printf("hfs_mountfs: DKIOCGETBLOCKCOUNT (2) failed\n");
			}
			retval = ENXIO;
			goto error_exit;
		}
	}

	/*
	 * At this point:
	 *   minblksize is the minimum physical block size
	 *   log_blksize has our preferred physical block size
	 *   log_blkcnt has the total number of physical blocks
	 */

	mdb_offset = (daddr_t)HFS_PRI_SECTOR(log_blksize);

	if ((retval = (int)bread(devvp,
				HFS_PHYSBLK_ROUNDDOWN(mdb_offset, (phys_blksize/log_blksize)), 
				phys_blksize, cred, &bp))) {
		if (HFS_MOUNT_DEBUG) {
			printf("hfs_mountfs: bread failed with %d\n", retval);
		}
		goto error_exit;
	}
	mdbp = hfs_malloc(kMDBSize);
	bcopy((char *)bp->b_data + HFS_PRI_OFFSET(phys_blksize), mdbp, kMDBSize);
	brelse(bp);
	bp = NULL;

	hfsmp = hfs_mallocz(sizeof(struct hfsmount));

	hfs_chashinit_finish(hfsmp);
	
	/* Init the ID lookup hashtable */
	hfs_idhash_init (hfsmp);

	/*
	 * See if the disk supports unmap (trim).
	 *
	 * NOTE: vfs_init_io_attributes has not been called yet, so we can't use the io_flags field
	 * returned by vfs_ioattr.  We need to call VNOP_IOCTL ourselves.
	 */
	if (VNOP_IOCTL(devvp, cp, DKIOCGETFEATURES, (caddr_t)&device_features, 0) == 0) {
		if (device_features & DK_FEATURE_UNMAP) {
			hfsmp->hfs_flags |= HFS_UNMAP;
		}

		if(device_features & DK_FEATURE_BARRIER)
			hfsmp->hfs_flags |= HFS_FEATURE_BARRIER;
	}

	/* 
	 * See if the disk is a solid state device, too.  We need this to decide what to do about 
	 * hotfiles.
	 */
    
	if (VNOP_IOCTL(devvp, cp, DKIOCISSOLIDSTATE, (caddr_t)&isssd, 0) == 0) {
		if (isssd) {
			hfsmp->hfs_flags |= HFS_SSD;
		}
	}

	/* See if the underlying device is Core Storage or not */
#if corestorage
	dk_corestorage_info_t cs_info;
	memset(&cs_info, 0, sizeof(dk_corestorage_info_t));
	if (VNOP_IOCTL(devvp, cp, DKIOCCORESTORAGE, (caddr_t)&cs_info, 0) == 0) {
		hfsmp->hfs_flags |= HFS_CS;
#if rootmount
		if (isroot && (cs_info.flags & DK_CORESTORAGE_PIN_YOUR_METADATA)) {
			hfsmp->hfs_flags |= HFS_CS_METADATA_PIN;
		}
		if (isroot && (cs_info.flags & DK_CORESTORAGE_ENABLE_HOTFILES)) {
			hfsmp->hfs_flags |= HFS_CS_HOTFILE_PIN;
			hfsmp->hfs_cs_hotfile_size = cs_info.hotfile_size;
		}
#endif // rootmount
		if ((cs_info.flags & DK_CORESTORAGE_PIN_YOUR_SWAPFILE)) {
			hfsmp->hfs_flags |= HFS_CS_SWAPFILE_PIN;

			struct vfsioattr ioattr;
			vfs_ioattr(mp, &ioattr);
			ioattr.io_flags |= VFS_IOATTR_FLAGS_SWAPPIN_SUPPORTED;
			ioattr.io_max_swappin_available = cs_info.swapfile_pinning;
			vfs_setioattr(mp, &ioattr);
		}
	}
#endif // corestorage

	/*
	 *  Init the volume information structure
	 */
	
    mtx_init(&hfsmp->hfs_mutex, "hfs_mutex", "hfs_mutex_group", MTX_DEF);
	lck_mtx_init(&hfsmp->hfc_mutex, hfs_mutex_group, hfs_lock_attr);
	lck_rw_init(&hfsmp->hfs_global_lock, hfs_rwlock_group, hfs_lock_attr);
	lck_spin_init(&hfsmp->vcbFreeExtLock, hfs_spinlock_group, hfs_lock_attr);

	if (mp) {
        mp->mnt_data = hfsmp;
    }
    
    hfsmp->hfs_cp = cp;
	hfsmp->hfs_mp = mp;			/* Make VFSTOHFS work */
	hfsmp->hfs_bo = bo;
	hfsmp->hfs_raw_dev = (devvp)->v_rdev;
	hfsmp->hfs_devvp = devvp;
	vref(devvp);  /* Hold a ref on the device, dropped when hfsmp is freed. */
	hfsmp->hfs_logical_block_size = log_blksize;
	hfsmp->hfs_logical_block_count = log_blkcnt;
	hfsmp->hfs_logical_bytes = (uint64_t) log_blksize * (uint64_t) log_blkcnt;
	hfsmp->hfs_physical_block_size = phys_blksize;
	hfsmp->hfs_log_per_phys = (phys_blksize / log_blksize);
	hfsmp->hfs_flags |= HFS_WRITEABLE_MEDIA;
	if (ronly)
		hfsmp->hfs_flags |= HFS_READ_ONLY;
	if (args && args->hfs_unknown_perms)
		hfsmp->hfs_flags |= HFS_UNKNOWN_PERMS;

#if QUOTA
	for (i = 0; i < MAXQUOTAS; i++)
		dqfileinit(&hfsmp->hfs_qfiles[i]);
#endif

	if (args) {
		hfsmp->hfs_uid = (args->hfs_uid == (uid_t)VNOVAL) ? UNKNOWNUID : args->hfs_uid;
		if (hfsmp->hfs_uid == 0xfffffffd) hfsmp->hfs_uid = UNKNOWNUID;
		hfsmp->hfs_gid = (args->hfs_gid == (gid_t)VNOVAL) ? UNKNOWNGID : args->hfs_gid;
		if (hfsmp->hfs_gid == 0xfffffffd) hfsmp->hfs_gid = UNKNOWNGID;
//		vfs_setowner(mp, hfsmp->hfs_uid, hfsmp->hfs_gid);				/* tell the VFS */
		if (args->hfs_mask != (mode_t)VNOVAL) {
			hfsmp->hfs_dir_mask = args->hfs_mask & ALLPERMS;
			if (args->hfs_noexec) {
				hfsmp->hfs_file_mask = (args->hfs_mask & DEFFILEMODE);
			} else {
				hfsmp->hfs_file_mask = args->hfs_mask & ALLPERMS;
			}
		} else {
			hfsmp->hfs_dir_mask = UNKNOWNPERMISSIONS & ALLPERMS;		/* 0777: rwx---rwx */
			hfsmp->hfs_file_mask = UNKNOWNPERMISSIONS & DEFFILEMODE;	/* 0666: no --x by default? */
		}
		if (args->hfs_wrapper)
			mntwrapper = 1;
	} else {
		/* Even w/o explicit mount arguments, MNT_UNKNOWNPERMISSIONS requires setting up uid, gid, and mask: */
			hfsmp->hfs_uid = UNKNOWNUID;
			hfsmp->hfs_gid = UNKNOWNGID;
//			vfs_setowner(mp, hfsmp->hfs_uid, hfsmp->hfs_gid);			/* tell the VFS */
			hfsmp->hfs_dir_mask = UNKNOWNPERMISSIONS & ALLPERMS;		/* 0777: rwx---rwx */
			hfsmp->hfs_file_mask = UNKNOWNPERMISSIONS & DEFFILEMODE;	/* 0666: no --x by default? */
	}

	/* Find out if disk media is writable. */
	if (VNOP_IOCTL(devvp, cp, DKIOCISWRITABLE, (caddr_t)&iswritable, 0) == 0) {
		if (iswritable)
			hfsmp->hfs_flags |= HFS_WRITEABLE_MEDIA;
		else
			hfsmp->hfs_flags &= ~HFS_WRITEABLE_MEDIA;
	}

	// Reservations
	rl_init(&hfsmp->hfs_reserved_ranges[0]);
	rl_init(&hfsmp->hfs_reserved_ranges[1]);

	// record the current time at which we're mounting this volume
	struct timeval tv;
	microtime(&tv);
	hfsmp->hfs_mount_time = tv.tv_sec;

	/* Mount a standard HFS disk */
	if ((SWAP_BE16(mdbp->drSigWord) == kHFSSigWord) &&
	    (mntwrapper || (SWAP_BE16(mdbp->drEmbedSigWord) != kHFSPlusSigWord))) {
#if CONFIG_HFS_STD 
		/* If only journal replay is requested, exit immediately */
		if (journal_replay_only) {
			retval = 0;
			goto error_exit;
		}

		/* On 10.6 and beyond, non read-only mounts for HFS standard vols get rejected */
		if (vfs_isrdwr(mp)) {
			retval = EROFS;
			goto error_exit;
		}

		printf("hfs_mountfs: Mounting HFS Standard volumes was deprecated in Mac OS 10.7 \n");

		/* Treat it as if it's read-only and not writeable */
		hfsmp->hfs_flags |= HFS_READ_ONLY;
		hfsmp->hfs_flags &= ~HFS_WRITEABLE_MEDIA;

		if ((mp->mnt_flag & MNT_ROOTFS)) {
			retval = EINVAL;  /* Cannot root from HFS standard disks */
			goto error_exit;
		}
		/* HFS disks can only use 512 byte physical blocks */
		if (log_blksize > kHFSBlockSize) {
			log_blksize = kHFSBlockSize;
			if (VNOP_IOCTL(devvp, cp, DKIOCSETBLOCKSIZE, (caddr_t)&log_blksize, 0)) {
				retval = ENXIO;
				goto error_exit;
			}
			if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKCOUNT, (caddr_t)&log_blkcnt, 0)) {
				retval = ENXIO;
				goto error_exit;
			}
			hfsmp->hfs_logical_block_size = log_blksize;
			hfsmp->hfs_logical_block_count = log_blkcnt;
			hfsmp->hfs_logical_bytes = (uint64_t) log_blksize * (uint64_t) log_blkcnt;
			hfsmp->hfs_physical_block_size = log_blksize;
			hfsmp->hfs_log_per_phys = 1;
		}
		if (args) {
			hfsmp->hfs_encoding = args->hfs_encoding;
			HFSTOVCB(hfsmp)->volumeNameEncodingHint = args->hfs_encoding;

			/* establish the timezone */
			gTimeZone = args->hfs_timezone;
		}

		retval = hfs_getconverter(hfsmp->hfs_encoding, &hfsmp->hfs_get_unicode, &hfsmp->hfs_get_hfsname);
		if (retval)
			goto error_exit;

		retval = hfs_MountHFSVolume(hfsmp, mdbp, p);
		if (retval)
			(void) hfs_relconverter(hfsmp->hfs_encoding);
#else
		/* On platforms where HFS Standard is not supported, deny the mount altogether */
		retval = EINVAL;
		goto error_exit;
#endif

	} 
	else { /* Mount an HFS Plus disk */
		HFSPlusVolumeHeader *vhp;
		off_t embeddedOffset;
		int   jnl_disable = 0;
	
		/* Get the embedded Volume Header */
		if (SWAP_BE16(mdbp->drEmbedSigWord) == kHFSPlusSigWord) {
			embeddedOffset = SWAP_BE16(mdbp->drAlBlSt) * kHFSBlockSize;
			embeddedOffset += (u_int64_t)SWAP_BE16(mdbp->drEmbedExtent.startBlock) *
			                  (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			/* 
			 * Cooperative Fusion is not allowed on embedded HFS+ 
			 * filesystems (HFS+ inside HFS standard wrapper)
			 */
			hfsmp->hfs_flags &= ~HFS_CS_METADATA_PIN;

			/*
			 * If the embedded volume doesn't start on a block
			 * boundary, then switch the device to a 512-byte
			 * block size so everything will line up on a block
			 * boundary.
			 */
			if ((embeddedOffset % log_blksize) != 0) {
				printf("hfs_mountfs: embedded volume offset not"
				    " a multiple of physical block size (%d);"
				    " switching to 512\n", log_blksize);
				log_blksize = 512;
                
				if (VNOP_IOCTL(devvp, cp, DKIOCSETBLOCKSIZE, (caddr_t)&log_blksize, 0)) {

					if (HFS_MOUNT_DEBUG) {
						printf("hfs_mountfs: DKIOCSETBLOCKSIZE (3) failed\n");
					}
					retval = ENXIO;
					goto error_exit;
				}
                
				if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKCOUNT, (caddr_t)&log_blkcnt, 0)) {
					if (HFS_MOUNT_DEBUG) {
						printf("hfs_mountfs: DKIOCGETBLOCKCOUNT (3) failed\n");
					}
					retval = ENXIO;
					goto error_exit;
				}
				/* Note: relative block count adjustment */
				hfsmp->hfs_logical_block_count *=
				    hfsmp->hfs_logical_block_size / log_blksize;
				
				/* Update logical /physical block size */
				hfsmp->hfs_logical_block_size = log_blksize;
				hfsmp->hfs_physical_block_size = log_blksize;
				
				phys_blksize = log_blksize;
				hfsmp->hfs_log_per_phys = 1;
			}

			disksize = (u_int64_t)SWAP_BE16(mdbp->drEmbedExtent.blockCount) *
			           (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			hfsmp->hfs_logical_block_count = disksize / log_blksize;
	
			hfsmp->hfs_logical_bytes = (uint64_t) hfsmp->hfs_logical_block_count * (uint64_t) hfsmp->hfs_logical_block_size;
			
			mdb_offset = (daddr_t)((embeddedOffset / log_blksize) + HFS_PRI_SECTOR(log_blksize));

			if (bp) {
				(bp)->b_flags |= B_INVAL;
				brelse(bp);
				bp = NULL;
			}
			retval = (int)bread(devvp, HFS_PHYSBLK_ROUNDDOWN(mdb_offset, hfsmp->hfs_log_per_phys),
					phys_blksize, cred, &bp);
			if (retval) {
				if (HFS_MOUNT_DEBUG) { 
					printf("hfs_mountfs: bread (2) failed with %d\n", retval);
				}
				goto error_exit;
			}
			bcopy((char *)bp->b_data + HFS_PRI_OFFSET(phys_blksize), mdbp, 512);
			brelse(bp);
			bp = NULL;
			vhp = (HFSPlusVolumeHeader*) mdbp;

		} 
		else { /* pure HFS+ */ 
			embeddedOffset = 0;
			vhp = (HFSPlusVolumeHeader*) mdbp;
		}

		retval = hfs_ValidateHFSPlusVolumeHeader(hfsmp, vhp);
		if (retval)
			goto error_exit;

		/*
		 * If allocation block size is less than the physical block size,
		 * invalidate the buffer read in using native physical block size
		 * to ensure data consistency.
		 *
		 * HFS Plus reserves one allocation block for the Volume Header.
		 * If the physical size is larger, then when we read the volume header,
		 * we will also end up reading in the next allocation block(s).
		 * If those other allocation block(s) is/are modified, and then the volume
		 * header is modified, the write of the volume header's buffer will write
		 * out the old contents of the other allocation blocks.
		 *
		 * We assume that the physical block size is same as logical block size.
		 * The physical block size value is used to round down the offsets for
		 * reading and writing the primary and alternate volume headers.
		 *
		 * The same logic is also in hfs_MountHFSPlusVolume to ensure that
		 * hfs_mountfs, hfs_MountHFSPlusVolume and later are doing the I/Os
		 * using same block size.
		 */
		if (SWAP_BE32(vhp->blockSize) < hfsmp->hfs_physical_block_size) {
			phys_blksize = hfsmp->hfs_logical_block_size;
			hfsmp->hfs_physical_block_size = hfsmp->hfs_logical_block_size;
			hfsmp->hfs_log_per_phys = 1;
			// There should be one bp associated with devvp in buffer cache.
			retval = buf_invalidateblks(devvp, 0, 0, 0);
			if (retval)
				goto error_exit;
		}
#if rootmount
		if (isroot && ((SWAP_BE32(vhp->attributes) & kHFSVolumeUnmountedMask) != 0)) {
			vfs_set_root_unmounted_cleanly();
		}
#endif
		/*
		 * On inconsistent disks, do not allow read-write mount
		 * unless it is the boot volume being mounted.  We also
		 * always want to replay the journal if the journal_replay_only
		 * flag is set because that will (most likely) get the
		 * disk into a consistent state before fsck_hfs starts
		 * looking at it.
		 */
		if (!journal_replay_only
			&& !(mp->mnt_flag & MNT_ROOTFS)
			&& (SWAP_BE32(vhp->attributes) & kHFSVolumeInconsistentMask)
			&& !(hfsmp->hfs_flags & HFS_READ_ONLY)) {
			
			if (HFS_MOUNT_DEBUG) { 
				printf("hfs_mountfs: failed to mount non-root inconsistent disk\n");
			}
			retval = EINVAL;
			goto error_exit;
		}


		// XXXdbg
		//
		hfsmp->jnl = NULL;
		hfsmp->jvp = NULL;
		if (args != NULL && args->journal_disable) {
		    jnl_disable = 1;
		}
				
		//
		// We only initialize the journal here if the last person
		// to mount this volume was journaling aware.  Otherwise
		// we delay journal initialization until later at the end
		// of hfs_MountHFSPlusVolume() because the last person who
		// mounted it could have messed things up behind our back
		// (so we need to go find the .journal file, make sure it's
		// the right size, re-sync up if it was moved, etc).
		//
		if (   (SWAP_BE32(vhp->lastMountedVersion) == kHFSJMountVersion)
			&& (SWAP_BE32(vhp->attributes) & kHFSVolumeJournaledMask)
			&& !jnl_disable) {
			
			// if we're able to init the journal, mark the mount
			// point as journaled.
			//
			if ((retval = hfs_early_journal_init(hfsmp, vhp, args, embeddedOffset, mdb_offset, mdbp, cred)) == 0) {
				if (mp)
					SET(mp->mnt_flag, (u_int64_t)((unsigned int)MNT_JOURNALED));
			} else {
				if (retval == EROFS) {
					// EROFS is a special error code that means the volume has an external
					// journal which we couldn't find.  in that case we do not want to
					// rewrite the volume header - we'll just refuse to mount the volume.
					if (HFS_MOUNT_DEBUG) { 
						printf("hfs_mountfs: hfs_early_journal_init indicated external jnl \n");
					}
					retval = EINVAL;
					goto error_exit;
				}

				// if the journal failed to open, then set the lastMountedVersion
				// to be "FSK!" which fsck_hfs will see and force the fsck instead
				// of just bailing out because the volume is journaled.
				if (!ronly) {
					if (HFS_MOUNT_DEBUG) { 
						printf("hfs_mountfs: hfs_early_journal_init failed, setting to FSK \n");
					}

					HFSPlusVolumeHeader *jvhp;

				    hfsmp->hfs_flags |= HFS_NEED_JNL_RESET;
				    
				    if (mdb_offset == 0) {
					mdb_offset = (daddr_t)((embeddedOffset / log_blksize) + HFS_PRI_SECTOR(log_blksize));
				    }

				    bp = NULL;
				    retval = (int)bread(devvp,
						    HFS_PHYSBLK_ROUNDDOWN(mdb_offset, hfsmp->hfs_log_per_phys), 
						    phys_blksize, cred, &bp);
				    if (retval == 0) {
					jvhp = (HFSPlusVolumeHeader *)(bp->b_data + HFS_PRI_OFFSET(phys_blksize));
					    
					if (SWAP_BE16(jvhp->signature) == kHFSPlusSigWord || SWAP_BE16(jvhp->signature) == kHFSXSigWord) {
						printf ("hfs(1): Journal replay fail.  Writing lastMountVersion as FSK!\n");
					    jvhp->lastMountedVersion = SWAP_BE32(kFSKMountVersion);
					    bwrite(bp);
					} else {
					    brelse(bp);
					}
					bp = NULL;
				    } else if (bp) {
					brelse(bp);
					// clear this so the error exit path won't try to use it
					bp = NULL;
				    }
				}

				// if this isn't the root device just bail out.
				// If it is the root device we just continue on
				// in the hopes that fsck_hfs will be able to
				// fix any damage that exists on the volume.
				if (mp && !(mp->mnt_flag & MNT_ROOTFS)) {
					if (HFS_MOUNT_DEBUG) { 
						printf("hfs_mountfs: hfs_early_journal_init failed, erroring out \n");
					}
				    retval = EINVAL;
				    goto error_exit;
				}
			}
		}

		/* Either the journal is replayed successfully, or there 
		 * was nothing to replay, or no journal exists.  In any case,
		 * return success.
		 */
		if (journal_replay_only) {
			retval = 0;
			goto error_exit;
		}

#if CONFIG_HFS_STD
		(void) hfs_getconverter(0, &hfsmp->hfs_get_unicode, &hfsmp->hfs_get_hfsname);
#endif

		retval = hfs_MountHFSPlusVolume(hfsmp, vhp, embeddedOffset, disksize, p, args, cred);
		/*
		 * If the backend didn't like our physical blocksize
		 * then retry with physical blocksize of 512.
		 */
		if ((retval == ENXIO) && (log_blksize > 512) && (log_blksize != minblksize)) {
			printf("hfs_mountfs: could not use physical block size "
					"(%d) switching to 512\n", log_blksize);
			log_blksize = 512;
            
			if (VNOP_IOCTL(devvp, cp, DKIOCSETBLOCKSIZE, (caddr_t)&log_blksize, 0)) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mountfs: DKIOCSETBLOCKSIZE (4) failed \n");
				}
				retval = ENXIO;
				goto error_exit;
			}
			if (VNOP_IOCTL(devvp, cp, DKIOCGETBLOCKCOUNT, (caddr_t)&log_blkcnt, 0)) {
				if (HFS_MOUNT_DEBUG) {
					printf("hfs_mountfs: DKIOCGETBLOCKCOUNT (4) failed \n");
				}
				retval = ENXIO;
				goto error_exit;
			}
//			set_fsblocksize(devvp);
			/* Note: relative block count adjustment (in case this is an embedded volume). */
			hfsmp->hfs_logical_block_count *= hfsmp->hfs_logical_block_size / log_blksize;
			hfsmp->hfs_logical_block_size = log_blksize;
			hfsmp->hfs_log_per_phys = hfsmp->hfs_physical_block_size / log_blksize;
	
			hfsmp->hfs_logical_bytes = (uint64_t) hfsmp->hfs_logical_block_count * (uint64_t) hfsmp->hfs_logical_block_size;

			if (hfsmp->jnl && hfsmp->jvp == devvp) {
			    // close and re-open this with the new block size
			    journal_close(hfsmp->jnl);
			    hfsmp->jnl = NULL;
			    if (hfs_early_journal_init(hfsmp, vhp, args, embeddedOffset, mdb_offset, mdbp, cred) == 0) {
					SET(mp->mnt_flag, (u_int64_t)((unsigned int)MNT_JOURNALED));
				} else {
					// if the journal failed to open, then set the lastMountedVersion
					// to be "FSK!" which fsck_hfs will see and force the fsck instead
					// of just bailing out because the volume is journaled.
					if (!ronly) {
						if (HFS_MOUNT_DEBUG) { 
							printf("hfs_mountfs: hfs_early_journal_init (2) resetting.. \n");
						}
				    	HFSPlusVolumeHeader *jvhp;

				    	hfsmp->hfs_flags |= HFS_NEED_JNL_RESET;
				    
				    	if (mdb_offset == 0) {
							mdb_offset = (daddr_t)((embeddedOffset / log_blksize) + HFS_PRI_SECTOR(log_blksize));
				    	}

				   	 	bp = NULL;
				    	retval = (int)bread(devvp, HFS_PHYSBLK_ROUNDDOWN(mdb_offset, hfsmp->hfs_log_per_phys),
							phys_blksize, cred, &bp);
				    	if (retval == 0) {
							jvhp = (HFSPlusVolumeHeader *)(bp->b_data + HFS_PRI_OFFSET(phys_blksize));
					    
							if (SWAP_BE16(jvhp->signature) == kHFSPlusSigWord || SWAP_BE16(jvhp->signature) == kHFSXSigWord) {
								printf ("hfs(2): Journal replay fail.  Writing lastMountVersion as FSK!\n");
					    		jvhp->lastMountedVersion = SWAP_BE32(kFSKMountVersion);
					    		bwrite(bp);
							} else {
					    		brelse(bp);
							}
							bp = NULL;
				    	} else if (bp) {
							brelse(bp);
							// clear this so the error exit path won't try to use it
							bp = NULL;
				    	}
					}

					// if this isn't the root device just bail out.
					// If it is the root device we just continue on
					// in the hopes that fsck_hfs will be able to
					// fix any damage that exists on the volume.
					if ( !(mp->mnt_flag & MNT_ROOTFS)) {
						if (HFS_MOUNT_DEBUG) { 
							printf("hfs_mountfs: hfs_early_journal_init (2) failed \n");
						}
				    	retval = EINVAL;
				    	goto error_exit;
					}
				}
			}

			/* Try again with a smaller block size... */
			retval = hfs_MountHFSPlusVolume(hfsmp, vhp, embeddedOffset, disksize, p, args, cred);
			if (retval && HFS_MOUNT_DEBUG) {
				printf("hfs_MountHFSPlusVolume (late) returned %d\n",retval); 
			}
		}
#if CONFIG_HFS_STD
		if (retval)
			(void) hfs_relconverter(0);
#endif
	}

	// save off a snapshot of the mtime from the previous mount
	// (for matador).
	hfsmp->hfs_last_mounted_mtime = hfsmp->hfs_mtime;

	if ( retval ) {
		if (HFS_MOUNT_DEBUG) { 
			printf("hfs_mountfs: encountered failure %d \n", retval);
		}
		goto error_exit;
	}

	struct statfs *vsfs = &(mp)->mnt_stat;
	vsfs->f_fsid.val[0] = (int32_t) dev2udev(dev);
	vsfs->f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;

//	vfs_setmaxsymlen(mp, 0);

#if CONFIG_HFS_STD
	if (ISSET(hfsmp->hfs_flags, HFS_STANDARD)) {
		/* HFS standard doesn't support extended readdir! */
		mount_set_noreaddirext (mp);
	}
#endif

	if (args) {
		/*
		 * Set the free space warning levels for a non-root volume:
		 *
		 * Set the "danger" limit to 1% of the volume size or 150MB, whichever is less.
		 * Set the "warning" limit to 2% of the volume size or 500MB, whichever is less.
		 * Set the "near warning" limit to 10% of the volume size or 1GB, whichever is less.
		 * And last, set the "desired" freespace level to to 12% of the volume size or 1.2GB,
		 * whichever is less.
		 */
		hfsmp->hfs_freespace_notify_dangerlimit =
			MIN(HFS_VERYLOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_VERYLOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_warninglimit =
			MIN(HFS_LOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_LOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_nearwarninglimit =
			MIN(HFS_NEARLOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_NEARLOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_desiredlevel =
			MIN(HFS_LOWDISKSHUTOFFLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_LOWDISKSHUTOFFFRACTION);
	} else {
		/*
		 * Set the free space warning levels for the root volume:
		 *
		 * Set the "danger" limit to 5% of the volume size or 512MB, whichever is less.
		 * Set the "warning" limit to 10% of the volume size or 1GB, whichever is less.
		 * Set the "near warning" limit to 10.5% of the volume size or 1.1GB, whichever is less.
		 * And last, set the "desired" freespace level to to 11% of the volume size or 1.25GB,
		 * whichever is less.
		 *
		 * NOTE: While those are the default limits, KernelEventAgent (as of 3/2016)
		 * will unilaterally override these to the following on OSX only:
		 *    Danger: 3GB
		 *    Warning: Min (2% of root volume, 10GB), with a floor of 10GB
		 *    Desired: Warning Threshold + 1.5GB  
		 */
		hfsmp->hfs_freespace_notify_dangerlimit =
			MIN(HFS_ROOTVERYLOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_ROOTVERYLOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_warninglimit =
			MIN(HFS_ROOTLOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_ROOTLOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_nearwarninglimit =
			MIN(HFS_ROOTNEARLOWDISKTRIGGERLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_ROOTNEARLOWDISKTRIGGERFRACTION);
		hfsmp->hfs_freespace_notify_desiredlevel =
			MIN(HFS_ROOTLOWDISKSHUTOFFLEVEL / HFSTOVCB(hfsmp)->blockSize,
				(HFSTOVCB(hfsmp)->totalBlocks / 100) * HFS_ROOTLOWDISKSHUTOFFFRACTION);
	};
	
	/* Check if the file system exists on virtual device, like disk image */
	if (VNOP_IOCTL(devvp, HFSTOCP(hfsmp), DKIOCISVIRTUAL, (caddr_t)&isvirtual, 0) == 0) {
		if (isvirtual) {
			hfsmp->hfs_flags |= HFS_VIRTUAL_DEVICE;
		}
	}

	if (
#if rootmount
        !isroot &&
#endif
		!ISSET(hfsmp->hfs_flags, HFS_VIRTUAL_DEVICE) &&
		hfs_is_ejectable(mp, devvp)) {
		SET(hfsmp->hfs_flags, HFS_RUN_SYNCER);
	}

	const char *dev_name = (hfsmp->hfs_devvp
							? devtoname(hfsmp->hfs_devvp->v_rdev) : "unknown device");

	printf("hfs: mounted %s on device %s\n",
		   (hfsmp->vcbVN[0] == 0? (const char*) hfsmp->vcbVN : "Untitled"), dev_name);
    
	/*
	 * Start looking for free space to drop below this level and generate a
	 * warning immediately if needed:
	 */
	hfsmp->hfs_notification_conditions = 0;
	hfs_generate_volume_notifications(hfsmp);

	if (ronly == 0) {
		(void) hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT);
	}
	hfs_free(mdbp, kMDBSize);
	return (0);

error_exit:
	if (bp != NULL)
		brelse(bp);

	if (cp != NULL){
		g_topology_lock();
		g_vfs_close(cp);
		g_topology_unlock();
	}

	hfs_free(mdbp, kMDBSize);

	hfs_close_jvp(hfsmp, td);

	if (hfsmp) {
		if (hfsmp->hfs_devvp) {
			vrele(hfsmp->hfs_devvp);
		}
		hfs_locks_destroy(hfsmp);
		hfs_delete_chash(hfsmp);
		hfs_idhash_destroy (hfsmp);

		hfs_free(hfsmp, sizeof(*hfsmp));
		if (mp)
			mp->mnt_data = NULL;
	}
	trace_return (retval);
}


/*
 * Make a filesystem operational.
 * Nothing to do at the moment.
 */
/* ARGSUSED */
static int __unused
hfs_start(__unused struct mount *mp, __unused int flags, __unused struct thread *td)
{
	return (0);
}


/*
 * unmount system call
 */
int
hfs_unmount(struct mount *mp, int mntflags)
{
	struct hfsmount *hfsmp = VFSTOHFS(mp);
	int retval = E_NONE;
	int flags;
	int force;
	int started_tr = 0;
    struct thread *td = curthread;

	flags = 0;
	force = 0;
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
		force = 1;
	}

	const char *dev_name = (hfsmp->hfs_devvp
							? devtoname(hfsmp->hfs_devvp->v_rdev) : NULL);

	printf("hfs: unmount initiated on %s on device %s\n",
		   (hfsmp->vcbVN[0] ? (const char*) hfsmp->vcbVN : "unknown"),
		   dev_name ?: "unknown device");
    
	if ((retval = hfs_flushfiles(mp, flags, td)) && !force)
 		trace_return (retval);

	if (hfsmp->hfs_flags & HFS_METADATA_ZONE)
		(void) hfs_recording_suspend(hfsmp);
    
	hfs_syncer_free(hfsmp);
    
	if (hfsmp->hfs_flags & HFS_SUMMARY_TABLE) {
		if (hfsmp->hfs_summary_table) {
			int err = 0;
			/* 
		 	 * Take the bitmap lock to serialize against a concurrent bitmap scan still in progress 
			 */
			if (hfsmp->hfs_allocation_vp) {
				err = hfs_lock (VTOC(hfsmp->hfs_allocation_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
			}
			hfs_free(hfsmp->hfs_summary_table, hfsmp->hfs_summary_bytes);
			hfsmp->hfs_summary_table = NULL;
			hfsmp->hfs_flags &= ~HFS_SUMMARY_TABLE;
			
			if (err == 0 && hfsmp->hfs_allocation_vp){
				hfs_unlock (VTOC(hfsmp->hfs_allocation_vp));
			}

		}
	}
	
	/*
	 * Flush out the b-trees, volume bitmap and Volume Header
	 */
	if ((hfsmp->hfs_flags & HFS_READ_ONLY) == 0) {
		retval = hfs_start_transaction(hfsmp);
		if (retval == 0) {
		    started_tr = 1;
		} else if (!force) {
		    goto err_exit;
		}

		if (hfsmp->hfs_startup_vp) {
			(void) hfs_lock(VTOC(hfsmp->hfs_startup_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
			retval = hfs_fsync(hfsmp->hfs_startup_vp, MNT_WAIT, 0, td);
			hfs_unlock(VTOC(hfsmp->hfs_startup_vp));
			if (retval && !force)
				goto err_exit;
		}

		if (hfsmp->hfs_attribute_vp) {
			(void) hfs_lock(VTOC(hfsmp->hfs_attribute_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
			retval = hfs_fsync(hfsmp->hfs_attribute_vp, MNT_WAIT, 0, td);
			hfs_unlock(VTOC(hfsmp->hfs_attribute_vp));
			if (retval && !force)
				goto err_exit;
		}

		(void) hfs_lock(VTOC(hfsmp->hfs_catalog_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
		retval = hfs_fsync(hfsmp->hfs_catalog_vp, MNT_WAIT, 0, td);
		hfs_unlock(VTOC(hfsmp->hfs_catalog_vp));
		if (retval && !force)
			goto err_exit;
		
		(void) hfs_lock(VTOC(hfsmp->hfs_extents_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
		retval = hfs_fsync(hfsmp->hfs_extents_vp, MNT_WAIT, 0, td);
		hfs_unlock(VTOC(hfsmp->hfs_extents_vp));
		if (retval && !force)
			goto err_exit;
			
		if (hfsmp->hfs_allocation_vp) {
			(void) hfs_lock(VTOC(hfsmp->hfs_allocation_vp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
			retval = hfs_fsync(hfsmp->hfs_allocation_vp, MNT_WAIT, 0, td);
			hfs_unlock(VTOC(hfsmp->hfs_allocation_vp));
			if (retval && !force)
				goto err_exit;
		}

		if (hfsmp->hfc_filevp && (hfsmp->hfc_filevp->v_vflag & VV_SYSTEM)) {
			retval = hfs_fsync(hfsmp->hfc_filevp, MNT_WAIT, 0, td);
			if (retval && !force)
				goto err_exit;
		}

		/* If runtime corruption was detected, indicate that the volume
		 * was not unmounted cleanly.
		 */
		if (hfsmp->vcbAtrb & kHFSVolumeInconsistentMask) {
			HFSTOVCB(hfsmp)->vcbAtrb &= ~kHFSVolumeUnmountedMask;
		} else {
			HFSTOVCB(hfsmp)->vcbAtrb |= kHFSVolumeUnmountedMask;
		}

		if (hfsmp->hfs_flags & HFS_HAS_SPARSE_DEVICE) {
			int i;
			u_int32_t min_start = hfsmp->totalBlocks;
			
			// set the nextAllocation pointer to the smallest free block number
			// we've seen so on the next mount we won't rescan unnecessarily
			lck_spin_lock(&hfsmp->vcbFreeExtLock);
			for(i=0; i < (int)hfsmp->vcbFreeExtCnt; i++) {
				if (hfsmp->vcbFreeExt[i].startBlock < min_start) {
					min_start = hfsmp->vcbFreeExt[i].startBlock;
				}
			}
			lck_spin_unlock(&hfsmp->vcbFreeExtLock);
			if (min_start < hfsmp->nextAllocation) {
				hfsmp->nextAllocation = min_start;
			}
		}

		retval = hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT);
		if (retval) {
			HFSTOVCB(hfsmp)->vcbAtrb &= ~kHFSVolumeUnmountedMask;
			if (!force)
				goto err_exit;	/* could not flush everything */
		}

		if (started_tr) {
		    hfs_end_transaction(hfsmp);
		    started_tr = 0;
		}
	}

	if (hfsmp->jnl) {
		hfs_flush(hfsmp, HFS_FLUSH_FULL);
	}
	
	/*
	 *	Invalidate our caches and release metadata vnodes
	 */
	(void) hfsUnmount(hfsmp, td);

#if CONFIG_HFS_STD
	if (HFSTOVCB(hfsmp)->vcbSigWord == kHFSSigWord) {
		(void) hfs_relconverter(hfsmp->hfs_encoding);
	}
#endif

	// XXXdbg
	if (hfsmp->jnl) {
	    journal_close(hfsmp->jnl);
	    hfsmp->jnl = NULL;
	}

	VOP_FSYNC(hfsmp->hfs_devvp, MNT_WAIT, td);

	hfs_close_jvp(hfsmp, td);

	/*
	 * Last chance to dump unreferenced system files.
	 */
	(void) vflush(mp, 0, FORCECLOSE, td);

#if HFS_SPARSE_DEV
	/* Drop our reference on the backing fs (if any). */
	if ((hfsmp->hfs_flags & HFS_HAS_SPARSE_DEVICE) && hfsmp->hfs_backingvp) {
		struct vnode * tmpvp;

		hfsmp->hfs_flags &= ~HFS_HAS_SPARSE_DEVICE;
		tmpvp = hfsmp->hfs_backingvp;
		hfsmp->hfs_backingvp = NULLVP;
		vrele(tmpvp);
	}
#endif /* HFS_SPARSE_DEV */

	g_topology_lock();
	g_vfs_close(hfsmp->hfs_cp);
	g_topology_unlock();

	vrele(hfsmp->hfs_devvp);

	hfs_locks_destroy(hfsmp);
	hfs_delete_chash(hfsmp);
	hfs_idhash_destroy(hfsmp);

	hfs_assert(TAILQ_EMPTY(&hfsmp->hfs_reserved_ranges[HFS_TENTATIVE_BLOCKS])
		   && TAILQ_EMPTY(&hfsmp->hfs_reserved_ranges[HFS_LOCKED_BLOCKS]));
	hfs_assert(!hfsmp->lockedBlocks);

	hfs_free(hfsmp, sizeof(*hfsmp));

	// decrement kext retain count
#if	TARGET_OS_OSX
	atomic_add_int((volatile u_int *)&hfs_active_mounts, -1);
//	OSKextReleaseKextWithLoadTag(OSKextGetCurrentLoadTag());
#endif

#if HFS_LEAK_DEBUG && TARGET_OS_OSX
	if (hfs_active_mounts == 0) {
		if (hfs_dump_allocations())
			panic("Oops");
		else {
			printf("hfs: last unmount and nothing was leaked!\n");
			msleep(hfs_unmount, NULL, PINOD, "hfs_unmount",
				   &(struct timespec){ 5, 0 });
		}
	}
#endif

	return (0);

  err_exit:
	if (started_tr) {
		hfs_end_transaction(hfsmp);
	}
	trace_return (retval);
}


/*
 * Return the root of a filesystem.
 */
int hfs_vfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	trace_return (hfs_vget(VFSTOHFS(mp), (cnid_t)kHFSRootFolderID, vpp, flags, 0));
}


/*
 * Do operations associated with quotas
 */
#if !QUOTA
static int
hfs_quotactl(struct mount *mp, int cmds, uid_t uid, void *arg)
{
	trace_return (ENOTSUP);
}
#else
static int
hfs_quotactl(struct mount *mp, int cmds, uid_t uid, void *datap)
{
	struct proc *p = td->td_proc;
	int cmd, type, error;

	if (uid == ~0U)
		uid = kauth_cred_getuid(td->td_ucred);
	cmd = cmds >> SUBCMDSHIFT;

	switch (cmd) {
	case Q_SYNC:
	case Q_QUOTASTAT:
		break;
	case Q_GETQUOTA:
		if (uid == kauth_cred_getuid(td->td_ucred))
			break;
		/* fall through */
	default:
		if ( (error = vfs_context_suser(context)) )
			trace_return (error);
	}

	type = cmds & SUBCMDMASK;
	if ((u_int)type >= MAXQUOTAS)
		trace_return (EINVAL);
	if ((error = vfs_busy(mp, LK_NOWAIT)) != 0)
		trace_return (error);

	switch (cmd) {

	case Q_QUOTAON:
		error = hfs_quotaon(p, mp, type, datap);
		break;

	case Q_QUOTAOFF:
		error = hfs_quotaoff(p, mp, type);
		break;

	case Q_SETQUOTA:
		error = hfs_setquota(mp, uid, type, datap);
		break;

	case Q_SETUSE:
		error = hfs_setuse(mp, uid, type, datap);
		break;

	case Q_GETQUOTA:
		error = hfs_getquota(mp, uid, type, datap);
		break;

	case Q_SYNC:
		error = hfs_qsync(mp);
		break;

	case Q_QUOTASTAT:
		error = hfs_quotastat(mp, type, datap);
		break;

	default:
		error = EINVAL;
		break;
	}
	vfs_unbusy(mp);

	trace_return (error);
}
#endif /* QUOTA */

/* Subtype is composite of bits */
#define HFS_SUBTYPE_JOURNALED      0x01
#define HFS_SUBTYPE_CASESENSITIVE  0x02
/* bits 2 - 6 reserved */
#define HFS_SUBTYPE_STANDARDHFS    0x80

/*
 * Get file system statistics.
 */
int
hfs_statfs(struct mount *mp, register struct statfs *sbp)
{
	ExtendedVCB *vcb = VFSTOVCB(mp);
	struct hfsmount *hfsmp = VFSTOHFS(mp);
	u_int16_t subtype = 0;

	sbp->f_bsize = (u_int32_t)vcb->blockSize;
	sbp->f_iosize = (size_t)mp->mnt_iosize_max;
	sbp->f_blocks = (u_int64_t)((u_int32_t)vcb->totalBlocks);
	sbp->f_bfree = (u_int64_t)((u_int32_t )hfs_freeblks(hfsmp, 0));
	sbp->f_bavail = (u_int64_t)((u_int32_t )hfs_freeblks(hfsmp, 1));
	sbp->f_files = (u_int64_t)HFS_MAX_FILES;
	sbp->f_ffree = (u_int64_t)hfs_free_cnids(hfsmp);

	/*
	 * Subtypes (flavors) for HFS
	 *   0:   Mac OS Extended
	 *   1:   Mac OS Extended (Journaled) 
	 *   2:   Mac OS Extended (Case Sensitive) 
	 *   3:   Mac OS Extended (Case Sensitive, Journaled) 
	 *   4 - 127:   Reserved
	 * 128:   Mac OS Standard
	 * 
	 */
	if ((hfsmp->hfs_flags & HFS_STANDARD) == 0) {
		/* HFS+ & variants */
		if (hfsmp->jnl) {
			subtype |= HFS_SUBTYPE_JOURNALED;
		}
		if (hfsmp->hfs_flags & HFS_CASE_SENSITIVE) {
			subtype |= HFS_SUBTYPE_CASESENSITIVE;
		}
	}
#if CONFIG_HFS_STD
	else {
		/* HFS standard */
		subtype = HFS_SUBTYPE_STANDARDHFS;
	} 
#endif
//	sbp->f_fssubtype = subtype;

#if CONFIG_HFS_STD
    if ((VTOHFS(ap->a_vp)->hfs_flags & HFS_STANDARD) != 0) {
        sbp->f_namemax = kHFSMaxFileNameChars;  /* 31 */
    } else
#endif
    {
        sbp->f_namemax = kHFSPlusMaxFileNameChars;  /* 255 */
    }
    
	return (0);
}


//
// XXXdbg -- this is a callback to be used by the journal to
//           get meta data blocks flushed out to disk.
//
// XXXdbg -- be smarter and don't flush *every* block on each
//           call.  try to only flush some so we don't wind up
//           being too synchronous.
//
void
hfs_sync_metadata(void *arg)
{
	struct mount *mp = (struct mount *)arg;
	struct hfsmount *hfsmp;
	ExtendedVCB *vcb;
	struct buf *	bp;
	int  retval;
	daddr_t priIDSector;
	hfsmp = VFSTOHFS(mp);
	vcb = HFSTOVCB(hfsmp);

	// now make sure the super block is flushed
	priIDSector = (daddr_t)((vcb->hfsPlusIOPosOffset / hfsmp->hfs_logical_block_size) +
				  HFS_PRI_SECTOR(hfsmp->hfs_logical_block_size));

	retval = (int)bread(hfsmp->hfs_devvp,
			HFS_PHYSBLK_ROUNDDOWN(priIDSector, hfsmp->hfs_log_per_phys),
			hfsmp->hfs_physical_block_size, NOCRED, &bp);
	if ((retval != 0 ) && (retval != ENXIO)) {
		printf("hfs_sync_metadata: can't read volume header at %d! (retval 0x%x)\n",
		       (int)priIDSector, retval);
	}

	if (retval == 0 && (bp->b_flags & B_DELWRI) && BUF_ISLOCKED(bp)) {
	    bwrite(bp);
	} else if (bp) {
	    brelse(bp);
	}
	
	/* Note that these I/Os bypass the journal (no calls to journal_start_modify_block) */

	// the alternate super block...
	// XXXdbg - we probably don't need to do this each and every time.
	//          hfs_btreeio.c:FlushAlternate() should flag when it was
	//          written...
	if (hfsmp->hfs_partition_avh_sector) {
		retval = (int)bread(hfsmp->hfs_devvp,
				HFS_PHYSBLK_ROUNDDOWN(hfsmp->hfs_partition_avh_sector, hfsmp->hfs_log_per_phys),
				hfsmp->hfs_physical_block_size, NOCRED, &bp);
		if (retval == 0 && (bp->b_flags & B_DELWRI) && BUF_ISLOCKED(bp)) {
		    /* 
			 * note this I/O can fail if the partition shrank behind our backs! 
			 * So failure should be OK here.
			 */
			bwrite(bp);
		} else if (bp) {
		    brelse(bp);
		}
	}

	/* Is the FS's idea of the AVH different than the partition ? */
	if ((hfsmp->hfs_fs_avh_sector) && (hfsmp->hfs_partition_avh_sector != hfsmp->hfs_fs_avh_sector)) {
		retval = (int)bread(hfsmp->hfs_devvp,
				HFS_PHYSBLK_ROUNDDOWN(hfsmp->hfs_fs_avh_sector, hfsmp->hfs_log_per_phys),
				hfsmp->hfs_physical_block_size, NOCRED, &bp);
		if (retval == 0 && (bp->b_flags & B_DELWRI) && BUF_ISLOCKED(bp)) {
		    bwrite(bp);
		} else if (bp) {
		    brelse(bp);
		}
	}

}

/*
 * Go through the disk queues to initiate sandbagged IO;
 * go through the inodes to write those that have been modified;
 * initiate the writing of the super block if it has been modified.
 *
 * Note: we are always called with the filesystem marked `MPBUSY'.
 */
int
hfs_sync(struct mount *mp, int waitfor)
{
    struct thread *td;
	struct cnode *cp;
	struct hfsmount *hfsmp;
	ExtendedVCB *vcb;
	struct vnode *meta_vp[4];
    struct vnode *vp, *mvp;
	int i;
	int error = 0;
    int atime_only_syncs = 0;
    time_t sync_start_time;
    struct timeval tv;
    struct ucred* cred;
    
    td = curthread;
    cred = td->td_ucred;
	hfsmp = VFSTOHFS(mp);
    microtime(&tv);
    sync_start_time = tv.tv_sec;

	// Back off if hfs_changefs or a freeze is underway
	hfs_lock_mount(hfsmp);
	if ((hfsmp->hfs_flags & HFS_IN_CHANGEFS)
	    || hfsmp->hfs_freeze_state != HFS_THAWED) {
		hfs_unlock_mount(hfsmp);
		return (0);
	}

	if (hfsmp->hfs_flags & HFS_READ_ONLY) {
		hfs_unlock_mount(hfsmp);
		trace_return (EROFS);
	}

	++hfsmp->hfs_syncers;
	hfs_unlock_mount(hfsmp);

	/*
	 * hfs_sync_callback will be called for each vnode
	 * hung off of this mount point... the vnode will be
	 * properly referenced and unreferenced around the callback
	 */
    MNT_VNODE_FOREACH_ALL(vp, mp, mvp) {
        struct cnode *cp = VTOC(vp);
        
        if (hfs_lock(cp, HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT) != 0) {
            VI_UNLOCK(vp);
        }
        
        hfs_dirty_t dirty_state = hfs_is_dirty(cp);
        
        bool sync = dirty_state == HFS_DIRTY || vp->v_bufobj.bo_dirty.bv_cnt > 0;
        
        if (!sync && dirty_state == HFS_DIRTY_ATIME
            && atime_only_syncs < 256) {
            // We only update if the atime changed more than 60s ago
            if (sync_start_time - cp->c_attr.ca_atime > 60) {
                sync = true;
                ++atime_only_syncs;
            }
        }
        
        if (sync) {
            error = hfs_fsync(vp, waitfor, 0, td);
        } else if (cp->c_touch_acctime)
            hfs_touchtimes(VTOHFS(vp), cp);
        
        hfs_unlock(cp);
    }
    
	vcb = HFSTOVCB(hfsmp);

	meta_vp[0] = vcb->extentsRefNum;
	meta_vp[1] = vcb->catalogRefNum;
	meta_vp[2] = vcb->allocationsRefNum;  /* This is NULL for standard HFS */
	meta_vp[3] = hfsmp->hfs_attribute_vp; /* Optional file */

	/* Now sync our three metadata files */
	for (i = 0; i < 4; ++i) {
		struct vnode *btvp;

		btvp = meta_vp[i];;
		if ((btvp==0) || ((btvp)->v_mount != mp))
			continue;

		/* FIXME: use hfs_systemfile_lock instead ? Or maybe vget(LK_EXCLUSIVE)? */
		(void) hfs_lock(VTOC(btvp), HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT);
		cp = VTOC(btvp);

		if (!hfs_is_dirty(cp) && vp->v_bufobj.bo_dirty.bv_cnt == 0) {
			hfs_unlock(VTOC(btvp));
			continue;
		}
		error = vget(btvp, 0);
		if (error) {
			hfs_unlock(VTOC(btvp));
			continue;
		}
		if ((error = hfs_fsync(btvp, waitfor, 0, td)))
			error = error;

		hfs_unlock(cp);
		vrele(btvp);
	};


#if CONFIG_HFS_STD
	/*
	 * Force stale file system control information to be flushed.
	 */
	if (vcb->vcbSigWord == kHFSSigWord) {
		if ((error = VOP_FSYNC(hfsmp->hfs_devvp, waitfor, context))) {
			error = error;
		}
	}
#endif

#if QUOTA
	hfs_qsync(mp);
#endif /* QUOTA */

	hfs_hotfilesync(hfsmp, td);

	/*
	 * Write back modified superblock.
	 */
	if (IsVCBDirty(vcb)) {
		error = hfs_flushvolumeheader(hfsmp, waitfor == MNT_WAIT ? HFS_FVH_WAIT : 0);
	}

	if (hfsmp->jnl) {
	    hfs_flush(hfsmp, HFS_FLUSH_JOURNAL);
	}

	hfs_lock_mount(hfsmp);
	boolean_t wake = (!--hfsmp->hfs_syncers && hfsmp->hfs_freeze_state == HFS_WANT_TO_FREEZE);
	hfs_unlock_mount(hfsmp);
	if (wake)
		wakeup(&hfsmp->hfs_freeze_state);

	trace_return (error);
}


/*
 * File handle to vnode
 *
 * Have to be really careful about stale file handles:
 * - check that the cnode id is valid
 * - call hfs_vget() to get the locked cnode
 * - check for an unallocated cnode (i_mode == 0)
 * - check that the given client host has export rights and return
 *   those rights via. exflagsp and credanonp
 */
static int
hfs_fhtovp(struct mount *mp, struct fid *fhp, int flags, struct vnode **vpp)
{
	struct hfsfid *hfsfhp;
	struct vnode *nvp;
	int result;

	*vpp = NULL;
	hfsfhp = (struct hfsfid *)fhp;

    result = hfs_vget(VFSTOHFS(mp), ntohl(hfsfhp->hfsfid_cnid), &nvp, flags, 0);
	if (result) {
		if (result == ENOENT)
			result = ESTALE;
		trace_return (result);
	}

	/* 
	 * We used to use the create time as the gen id of the file handle,
	 * but it is not static enough because it can change at any point 
	 * via system calls.  We still don't have another volume ID or other
	 * unique identifier to use for a generation ID across reboots that
	 * persists until the file is removed.  Using only the CNID exposes
	 * us to the potential wrap-around case, but as of 2/2008, it would take
	 * over 2 months to wrap around if the machine did nothing but allocate
	 * CNIDs.  Using some kind of wrap counter would only be effective if
	 * each file had the wrap counter associated with it.  For now, 
	 * we use only the CNID to identify the file as it's good enough.
	 */	 

	*vpp = nvp;

	return (0);
}


/*
 * Vnode pointer to File handle
 */
/* ARGSUSED */
int
hfs_vptofh(struct vop_vptofh_args *ap)
{
    struct vnode *vp = ap->a_vp;
	struct cnode *cp;
	struct hfsfid *hfsfhp;

	if (ISHFS(VTOVCB(vp)))
		trace_return (ENOTSUP);	/* hfs standard is not exportable */

	cp = VTOC(vp);
	hfsfhp = (struct hfsfid *)ap->a_fhp;
	/* only the CNID is used to identify the file now */
	hfsfhp->hfsfid_cnid = htonl(cp->c_fileid);
	hfsfhp->hfsfid_gen = htonl(cp->c_fileid);
	
	return (0);
}


/*
 * Initialize HFS filesystems, done only once per boot.
 *
 * HFS is not a kext-based file system.  This makes it difficult to find 
 * out when the last HFS file system was unmounted and call hfs_uninit() 
 * to deallocate data structures allocated in hfs_init().  Therefore we 
 * never deallocate memory allocated by lock attribute and group initializations 
 * in this function.
 */
static int done = 0;

int
hfs_init(__unused struct vfsconf *vfsp)
{
	if (done)
		return (0);
	done = 1;
	hfs_chashinit();

	BTReserveSetup();
	
	hfs_lock_attr    = lck_attr_alloc_init();
	hfs_group_attr   = lck_grp_attr_alloc_init();
	hfs_mutex_group  = lck_grp_alloc_init("hfs-mutex", hfs_group_attr);
	hfs_rwlock_group = lck_grp_alloc_init("hfs-rwlock", hfs_group_attr);
	hfs_spinlock_group = lck_grp_alloc_init("hfs-spinlock", hfs_group_attr);
	
#if HFS_COMPRESSION
	decmpfs_init();
#endif

	journal_init();

	return (0);
}

int
hfs_uninit(__unused struct vfsconf *vfsp)
{
	hfs_assert(done == 1); 

	hfs_chashdestroy();

	BTReserveDestroy();
	
	lck_grp_free(hfs_mutex_group);
	lck_grp_free(hfs_rwlock_group);
	lck_grp_free(hfs_spinlock_group);
	lck_attr_free(hfs_lock_attr);
	lck_grp_attr_free(hfs_group_attr);
	
#if HFS_COMPRESSION
	decmpfs_uninit();
#endif

	journal_uninit();

	return (0);
}


/*
 * Destroy all locks, mutexes and spinlocks in hfsmp on unmount or failed mount
 */ 
static void 
hfs_locks_destroy(struct hfsmount *hfsmp)
{

	mtx_destroy(&hfsmp->hfs_mutex);
	lck_mtx_destroy(&hfsmp->hfc_mutex, hfs_mutex_group);
	lck_rw_destroy(&hfsmp->hfs_global_lock, hfs_rwlock_group);
	lck_spin_destroy(&hfsmp->vcbFreeExtLock, hfs_spinlock_group);

	return;
}


__unused
static int
hfs_getmountpoint(struct vnode *vp, struct hfsmount **hfsmpp)
{
	struct hfsmount * hfsmp;
    char *fstypename;

	if (vp == NULL)
		trace_return (EINVAL);
	
	if (!(vp->v_vflag & VV_ROOT))
		trace_return (EINVAL);
    
    fstypename = vp->v_mount->mnt_vfc->vfc_name;
    
	if (strncmp(fstypename, "hfs", MFSNAMELEN) != 0)
		trace_return (EINVAL);

	hfsmp = VTOHFS(vp);

	if (HFSTOVCB(hfsmp)->vcbSigWord == kHFSSigWord)
		trace_return (EINVAL);

	*hfsmpp = hfsmp;

	return (0);
}

// Replace user-space value
__unused
static int ureplace(caddr_t oldp, size_t *oldlenp,
						caddr_t newp, size_t newlen,
						void *data, size_t len)
{
	int error;
	if (!oldlenp)
		return EFAULT;
	if (oldp && *oldlenp < len)
		return ENOMEM;
	if (newp && newlen != len)
		return EINVAL;
	*oldlenp = len;
	if (oldp) {
		error = copyout(data, oldp, len);
		if (error)
			return error;
	}
	return newp ? copyin(newp, data, len) : 0;
}

#define UREPLACE(oldp, oldlenp, newp, newlenp, v)	\
	ureplace(oldp, oldlenp, newp, newlenp, &v, sizeof(v))

__unused
static hfsmount_t *hfs_mount_from_cwd(struct thread * td)
{
    struct pwd *pwd;
    struct vnode* vp;
    
    pwd = pwd_get_smr();
    vp = pwd->pwd_cdir;
    
	if (!vp)
		return NULL;

	/*
	 * We could use vnode_tag, but it is probably more future proof to
	 * compare fstypename.
	 */

	if (strncmp(vp->v_mount->mnt_vfc->vfc_name, "hfs", MFSNAMELEN))
		return NULL;

	return VTOHFS(vp);
}

/*
 * HFS filesystem related variables.
 */
int
hfs_sysctl(struct mount *mp, fsctlop_t op,struct sysctl_req *req)
{
	__unused int error;
	__unused struct hfsmount *hfsmp;
	__unused struct proc *p = NULL;
    __unused int *name;
    __unused u_int namelen;
    __unused caddr_t oldp = req->oldptr;
    __unused size_t oldlen = req->oldlen;
    __unused const void* newp = req->newptr;
    __unused size_t newlen = req->newlen;
    __unused struct thread *td = req->td;

#if 0
    
	/* all sysctl names at this level are terminal */
#if  TARGET_OS_OSX
	p = td->td_proc;
	if (op == HFS_ENCODINGBIAS) {
		int bias;

        if (req->oldptr != NULL) {
            bias = hfs_getencodingbias();
            
            error = SYSCTL_IN(req, &bias, sizeof(bias));
            if (error)
                trace_return (error);
            
            hfs_setencodingbias(bias);
        }
        
		return 0;
	} else
#endif //OSX
	if (op == HFS_EXTEND_FS) {
		u_int64_t  newsize = 0;
        struct pwd* pwd = pwd_get_smr();
		struct vnode* vp = pwd->pwd_cdir;

		if (req->newptr == NULL || vp == NULLVP || req->newlen != sizeof(quad_t))
			return EINVAL;
        
		if ((error = hfs_getmountpoint(vp, &hfsmp)))
			trace_return (error);

		/* Start with the 'size' set to the current number of bytes in the filesystem */
		newsize = ((uint64_t)hfsmp->totalBlocks) * ((uint64_t)hfsmp->blockSize);

        error = SYSCTL_IN(req, &newsize, sizeof(newsize));
		if (error)
			return error;

		return hfs_extendfs(hfsmp, newsize, td);
	} else if (op == HFS_ENABLE_JOURNALING) {
		// make the file system journaled...
		struct vnode* jvp;
		ExtendedVCB *vcb;
		struct cat_attr jnl_attr;
	    struct cat_attr	jinfo_attr;
		struct cat_fork jnl_fork;
		struct cat_fork jinfo_fork;
		struct buf * jib_buf;
		uint64_t jib_blkno;
		uint32_t tmpblkno;
		uint64_t journal_byte_offset;
		uint64_t journal_size;
		struct vnode* jib_vp = NULLVP;
		struct JournalInfoBlock local_jib;
		int __unused err = 0;
		void *jnl = NULL;
		int lockflags;

		/* Only root can enable journaling */
		if (!priv_check_cred(td->td_ucred, PRIV_VFS_ADMIN)) {
			trace_return (EPERM);
		}
        
		if (namelen != 4)
			return EINVAL;
		hfsmp = hfs_mount_from_cwd(td);
		if (!hfsmp)
			return EINVAL;

		if (hfsmp->hfs_flags & HFS_READ_ONLY) {
			return EROFS;
		}
		if (HFSTOVCB(hfsmp)->vcbSigWord == kHFSSigWord) {
			printf("hfs: can't make a plain hfs volume journaled.\n");
			return EINVAL;
		}

		if (hfsmp->jnl) {
		    printf("hfs: volume %s is already journaled!\n", hfsmp->vcbVN);
		    return EAGAIN;
		}
		vcb = HFSTOVCB(hfsmp);

		/* Set up local copies of the initialization info */
		tmpblkno = (uint32_t) name[1];
		jib_blkno = (uint64_t) tmpblkno;
		journal_byte_offset = (uint64_t) name[2];
		journal_byte_offset *= hfsmp->blockSize;
		journal_byte_offset += hfsmp->hfsPlusIOPosOffset;
		journal_size = (uint64_t)((unsigned)name[3]);

		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG | SFL_EXTENTS, HFS_EXCLUSIVE_LOCK);
		if (BTHasContiguousNodes(VTOF(vcb->catalogRefNum)) == 0 ||
			BTHasContiguousNodes(VTOF(vcb->extentsRefNum)) == 0) {

			printf("hfs: volume has a btree w/non-contiguous nodes.  can not enable journaling.\n");
			hfs_systemfile_unlock(hfsmp, lockflags);
			return EINVAL;
		}
		hfs_systemfile_unlock(hfsmp, lockflags);

		// make sure these both exist!
		if (   GetFileInfo(vcb, kHFSRootFolderID, ".journal_info_block", &jinfo_attr, &jinfo_fork) == 0
			|| GetFileInfo(vcb, kHFSRootFolderID, ".journal", &jnl_attr, &jnl_fork) == 0) {

			return EINVAL;
		}

		/*
		 * At this point, we have a copy of the metadata that lives in the catalog for the
		 * journal info block.  Compare that the journal info block's single extent matches
		 * that which was passed into this sysctl.  
		 *
		 * If it is different, deny the journal enable call.
		 */
		if (jinfo_fork.cf_blocks > 1) {
			/* too many blocks */
			return EINVAL;
		}

		if (jinfo_fork.cf_extents[0].startBlock != jib_blkno) {
			/* Wrong block */
			return EINVAL;
		}

		/*   
		 * We want to immediately purge the vnode for the JIB.
		 * 
		 * Because it was written to from userland, there's probably 
		 * a vnode somewhere in the vnode cache (possibly with UBC backed blocks). 
		 * So we bring the vnode into core, then immediately do whatever 
		 * we can to flush/vclean it out.  This is because those blocks will be 
		 * interpreted as user data, which may be treated separately on some platforms
		 * than metadata.  If the vnode is gone, then there cannot be backing blocks
		 * in the UBC.
		 */
		if (hfs_vget (hfsmp, jinfo_attr.ca_fileid, &jib_vp, 1, 0)) {
			return EINVAL;
		} 
		/*
		 * Now we have a vnode for the JIB. recycle it. Because we hold an iocount
		 * on the vnode, we'll just mark it for termination when the last iocount
		 * (hopefully ours), is dropped.
		 */
		vrecycle (jib_vp);
		vput (jib_vp);

		/* Initialize the local copy of the JIB (just like hfs.util) */
		memset (&local_jib, 'Z', sizeof(struct JournalInfoBlock));
		local_jib.flags = SWAP_BE32(kJIJournalInFSMask);
		/* Note that the JIB's offset is in bytes */
		local_jib.offset = SWAP_BE64(journal_byte_offset);
		local_jib.size = SWAP_BE64(journal_size);  

		/* 
		 * Now write out the local JIB.  This essentially overwrites the userland
		 * copy of the JIB.  Read it as BLK_META to treat it as a metadata read/write.
		 */
		jib_buf = getblk (hfsmp->hfs_devvp,
				jib_blkno * (hfsmp->blockSize / hfsmp->hfs_logical_block_size), 
				hfsmp->blockSize, 0, 0, 0);
		char* buf_ptr = (char*) (jib_buf)->b_data;

		/* Zero out the portion of the block that won't contain JIB data */
		memset (buf_ptr, 0, hfsmp->blockSize);

		bcopy(&local_jib, buf_ptr, sizeof(local_jib));
		if (bwrite (jib_buf)) {
			return EIO;
		}

		/* Force a flush track cache */
		hfs_flush(hfsmp, HFS_FLUSH_CACHE);

		/* Now proceed with full volume sync */
		hfs_sync(hfsmp->hfs_mp, MNT_WAIT);

        printf("hfs: Initializing the journal (joffset 0x%lx sz 0x%llx)...\n",
			   (off_t)name[2], (off_t)name[3]);

		//
		// XXXdbg - note that currently (Sept, 08) hfs_util does not support
		//          enabling the journal on a separate device so it is safe
		//          to just copy hfs_devvp here.  If hfs_util gets the ability
		//          to dynamically enable the journal on a separate device then
		//          we will have to do the same thing as hfs_early_journal_init()
		//          to locate and open the journal device.
		//
		jvp = hfsmp->hfs_devvp;
		jnl = journal_create(jvp, journal_byte_offset, journal_size, 
							 hfsmp->hfs_devvp,
							 hfsmp->hfs_logical_block_size,
							 0,
							 0,
							 hfs_sync_metadata, hfsmp->hfs_mp,
							 hfsmp->hfs_mp);

		/*
		 * Set up the trim callback function so that we can add
		 * recently freed extents to the free extent cache once
		 * the transaction that freed them is written to the
		 * journal on disk.
		 */
		if (jnl)
			journal_trim_set_callback(jnl, hfs_trim_callback, hfsmp);

		if (jnl == NULL) {
			printf("hfs: FAILED to create the journal!\n");
			return EIO;
		} 

		hfs_lock_global (hfsmp, HFS_EXCLUSIVE_LOCK);

		/*
		 * Flush all dirty metadata buffers.
		 */
		buf_flushdirtyblks(hfsmp->hfs_devvp, TRUE, 0, "hfs_sysctl");
		buf_flushdirtyblks(hfsmp->hfs_extents_vp, TRUE, 0, "hfs_sysctl");
		buf_flushdirtyblks(hfsmp->hfs_catalog_vp, TRUE, 0, "hfs_sysctl");
		buf_flushdirtyblks(hfsmp->hfs_allocation_vp, TRUE, 0, "hfs_sysctl");
		if (hfsmp->hfs_attribute_vp)
			buf_flushdirtyblks(hfsmp->hfs_attribute_vp, TRUE, 0, "hfs_sysctl");

		HFSTOVCB(hfsmp)->vcbJinfoBlock = name[1];
		HFSTOVCB(hfsmp)->vcbAtrb |= kHFSVolumeJournaledMask;
		hfsmp->jvp = jvp;
		hfsmp->jnl = jnl;

		// save this off for the hack-y check in hfs_remove()
		hfsmp->jnl_start        = (u_int32_t)name[2];
		hfsmp->jnl_size         = (uint32_t)((unsigned)name[3]);
		hfsmp->hfs_jnlinfoblkid = jinfo_attr.ca_fileid;
		hfsmp->hfs_jnlfileid    = jnl_attr.ca_fileid;

		SET(hfsmp->hfs_mp->mnt_flag, (u_int64_t)((unsigned int)MNT_JOURNALED));

		hfs_unlock_global (hfsmp);
		hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT | HFS_FVH_WRITE_ALT);

		{
			fsid_t fsid;
		
			fsid.val[0] = (int32_t)dev2udev(hfsmp->hfs_raw_dev);
			fsid.val[1] = (int32_t)(HFSTOVFS(hfsmp)->mnt_vfc->vfc_typenum);
//			vfs_event_signal(&fsid, VQ_UPDATE, (intptr_t)NULL); // FIXME: ...
		}
		return 0;
	} else if (op == HFS_DISABLE_JOURNALING) {
		// clear the journaling bit

		/* Only root can disable journaling */
		if (!priv_check_cred(td->td_ucred, PRIV_VFS_ADMIN)) {
			trace_return (EPERM);
		}

		hfsmp = hfs_mount_from_cwd(td);
		if (!hfsmp)
			return EINVAL;

		/* 
		 * Disabling journaling is disallowed on volumes with directory hard links
		 * because we have not tested the relevant code path.
		 */  
		if (hfsmp->hfs_private_attr[DIR_HARDLINKS].ca_entries != 0){
			printf("hfs: cannot disable journaling on volumes with directory hardlinks\n");
			return EPERM;
		}

		printf("hfs: disabling journaling for %s\n", hfsmp->vcbVN);

		hfs_lock_global (hfsmp, HFS_EXCLUSIVE_LOCK);

		// Lights out for you buddy!
		journal_close(hfsmp->jnl);
		hfsmp->jnl = NULL;

		hfs_close_jvp(hfsmp, td);
		CLR(hfsmp->hfs_mp->mnt_flag, (u_int64_t)((unsigned int)MNT_JOURNALED));
		hfsmp->jnl_start        = 0;
		hfsmp->hfs_jnlinfoblkid = 0;
		hfsmp->hfs_jnlfileid    = 0;
		
		HFSTOVCB(hfsmp)->vcbAtrb &= ~kHFSVolumeJournaledMask;
		
		hfs_unlock_global (hfsmp);

		hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT | HFS_FVH_WRITE_ALT);

		{
			fsid_t fsid;
		
			fsid.val[0] = (int32_t)dev2udev(hfsmp->hfs_raw_dev);
			fsid.val[1] = (int32_t)(HFSTOVFS(hfsmp)->mnt_vfc->vfc_typenum);
//			vfs_event_signal(&fsid, VQ_UPDATE, (intptr_t)NULL); // FIXME: ...
		}
		return 0;
	} else if (op == VFS_CTL_QUERY) {
#if TARGET_OS_IPHONE 
		return EPERM;
#else //!TARGET_OS_IPHONE
    	struct sysctl_req *req;
    	struct vfsidctl vc;
    	struct mount *mp;
 	    struct vfsquery vq;
	
        // FIXME: ...
        req = oldp; // CAST_DOWN(struct sysctl_req *, oldp);	/* we're new style vfs sysctl. */
		if (req == NULL) {
			return EFAULT;
		}
        
        error = SYSCTL_IN(req, &vc, sizeof(vc));
		if (error) return (error);

		mp = vfs_getvfs(&vc.vc_fsid); /* works for 32 and 64 */
        if (mp == NULL) trace_return (ENOENT);
        
		hfsmp = VFSTOHFS(mp);
		bzero(&vq, sizeof(vq));
		vq.vq_flags = hfsmp->hfs_notification_conditions;
		return SYSCTL_OUT(req, &vq, sizeof(vq));;
#endif // TARGET_OS_IPHONE
	} else if (op == HFS_REPLAY_JOURNAL) {
		struct vnode* devvp = NULL;
		int device_fd;
        
//		if (namelen != 2) {
//			trace_return (EINVAL);
//		}
//
//		device_fd = name[1];
        error = fgetvp(td, device_fd, &cap_read_rights, &devvp);
		if (error) {
			return error;
		}
        
		error = hfs_journal_replay(devvp, td);
		vrele(devvp);
		return error;
	}
#if DEBUG || TARGET_OS_OSX
	else if (op == HFS_ENABLE_RESIZE_DEBUG) {
		if (!priv_check_cred(td->td_ucred, PRIV_VFS_ADMIN)) {
			trace_return (EPERM);
		}

		int old = hfs_resize_debug;

		int res = UREPLACE(oldp, oldlenp, newp, newlen, hfs_resize_debug);

		if (old != hfs_resize_debug) {
			printf("hfs: %s resize debug\n",
				   hfs_resize_debug ? "enabled" : "disabled");
		}

		return res;
	}
#endif // DEBUG || OSX

#endif // disable
	trace_return (ENOTSUP);
}

/* 
 * hfs_vfs_vget is not static since it is used in hfs_readwrite.c to support
 * the vn_getpath_ext.  We use it to leverage the code below that updates
 * the origin list cache if necessary
 */

int
hfs_vfs_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp)
{
	int error;
	int lockflags;
	struct hfsmount *hfsmp;

	hfsmp = VFSTOHFS(mp);

	error = hfs_vget(hfsmp, (cnid_t)ino, vpp, flags, 0);
	if (error)
		trace_return (error);

	/*
	 * If the look-up was via the object ID (rather than the link ID),
	 * then we make sure there's a parent here.  We can't leave this
	 * until hfs_vnop_getattr because if there's a problem getting the
	 * parent at that point, all the caller will do is call
	 * hfs_vfs_vget again and we'll end up in an infinite loop.
	 */

    cnode_t *cp = VTOC(*vpp);

	if (ISSET(cp->c_flag, C_HARDLINK) && ino == cp->c_fileid) {
		hfs_lock_always(cp, HFS_SHARED_LOCK);

		if (!hfs_haslinkorigin(cp)) {
			if (!hfs_lock_upgrade(cp))
				hfs_lock_always(cp, HFS_EXCLUSIVE_LOCK);

			if (cp->c_cnid == cp->c_fileid) {
				/*
				 * Descriptor is stale, so we need to refresh it.  We
				 * pick the first link.
				 */
				cnid_t link_id;

				error = hfs_first_link(hfsmp, cp, &link_id);

				if (!error) {
					lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
					error = cat_findname(hfsmp, link_id, &cp->c_desc);
					hfs_systemfile_unlock(hfsmp, lockflags);
				}
			} else {
				// We'll use whatever link the descriptor happens to have
				error = 0;
			}
			if (!error)
				hfs_savelinkorigin(cp, cp->c_parentcnid);
		}

		hfs_unlock(cp);

		if (error) {
			vrele(*vpp);
			*vpp = NULL;
		}
	}

	trace_return (error);
}


/*
 * Look up an HFS object by ID.
 *
 * The object is returned with an iocount reference and the cnode locked.
 *
 * If the object is a file then it will represent the data fork.
 */
int
hfs_vget(struct hfsmount *hfsmp, cnid_t cnid, struct vnode **vpp, int lkflags, int allow_deleted)
{
	struct vnode *vp = NULLVP;
	struct cat_desc cndesc;
	struct cat_attr cnattr;
	struct cat_fork cnfork;
	u_int32_t linkref = 0;
	int error;
    struct gnv_flags gflags = {0};
    gflags.lkflags = lkflags;
    
	/* Check for cnids that should't be exported. */
	if ((cnid < kHFSFirstUserCatalogNodeID) &&
	    (cnid != kHFSRootFolderID && cnid != kHFSRootParentID)) {
		trace_return (ENOENT);
	}
	/* Don't export our private directories. */
	if (cnid == hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid ||
	    cnid == hfsmp->hfs_private_desc[DIR_HARDLINKS].cd_cnid) {
		trace_return (ENOENT);
	}
	/*
	 * Check the hash first
	 */
	vp = hfs_chash_getvnode(hfsmp, cnid, 0, lkflags, allow_deleted);
	if (vp) {
		*vpp = vp;
		return(0);
	}

	bzero(&cndesc, sizeof(cndesc));
	bzero(&cnattr, sizeof(cnattr));
	bzero(&cnfork, sizeof(cnfork));

	/*
	 * Not in hash, lookup in catalog
	 */
	if (cnid == kHFSRootParentID) {
		static char hfs_rootname[] = "/";

		cndesc.cd_nameptr = (const u_int8_t *)&hfs_rootname[0];
		cndesc.cd_namelen = 1;
		cndesc.cd_parentcnid = kHFSRootParentID;
		cndesc.cd_cnid = kHFSRootFolderID;
		cndesc.cd_flags = CD_ISDIR;

		cnattr.ca_fileid = kHFSRootFolderID;
		cnattr.ca_linkcount = 1;
		cnattr.ca_entries = 1;
		cnattr.ca_dircount = 1;
		cnattr.ca_mode = (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO);
	} else {
		int lockflags;
		cnid_t pid;
		const char *nameptr;

		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
		error = cat_idlookup(hfsmp, cnid, 0, 0, &cndesc, &cnattr, &cnfork);
		hfs_systemfile_unlock(hfsmp, lockflags);

		if (error) {
			*vpp = NULL;
			return (error);
		}

		/*
		 * Check for a raw hardlink inode and save its linkref.
		 */
		pid = cndesc.cd_parentcnid;
		nameptr = (const char *)cndesc.cd_nameptr;

		if ((pid == hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid) &&
			cndesc.cd_namelen > HFS_INODE_PREFIX_LEN &&
		    (bcmp(nameptr, HFS_INODE_PREFIX, HFS_INODE_PREFIX_LEN) == 0)) {
			linkref = (uint32_t) strtoul(&nameptr[HFS_INODE_PREFIX_LEN], NULL, 10);

		} else if ((pid == hfsmp->hfs_private_desc[DIR_HARDLINKS].cd_cnid) &&
				   cndesc.cd_namelen > HFS_DIRINODE_PREFIX_LEN &&
		           (bcmp(nameptr, HFS_DIRINODE_PREFIX, HFS_DIRINODE_PREFIX_LEN) == 0)) {
			linkref = (uint32_t) strtoul(&nameptr[HFS_DIRINODE_PREFIX_LEN], NULL, 10);

		} else if ((pid == hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid) &&
				   cndesc.cd_namelen > HFS_DELETE_PREFIX_LEN &&
		           (bcmp(nameptr, HFS_DELETE_PREFIX, HFS_DELETE_PREFIX_LEN) == 0)) {
			*vpp = NULL;
			cat_releasedesc(&cndesc);
			trace_return (ENOENT);  /* open unlinked file */
		}
	}

	/*
	 * Finish initializing cnode descriptor for hardlinks.
	 *
	 * We need a valid name and parent for reverse lookups.
	 */
	if (linkref) {
		cnid_t lastid;
		struct cat_desc linkdesc;
		int linkerr = 0;
		
		cnattr.ca_linkref = linkref;
		bzero (&linkdesc, sizeof (linkdesc));

		/* 
		 * If the caller supplied the raw inode value, then we don't know exactly
		 * which hardlink they wanted. It's likely that they acquired the raw inode
		 * value BEFORE the item became a hardlink, in which case, they probably
		 * want the oldest link.  So request the oldest link from the catalog.
		 * 
		 * Unfortunately, this requires that we iterate through all N hardlinks. On the plus
		 * side, since we know that we want the last linkID, we can also have this one
		 * call give us back the name of the last ID, since it's going to have it in-hand...
		 */
		linkerr = hfs_lookup_lastlink (hfsmp, linkref, &lastid, &linkdesc);
		if ((linkerr == 0) && (lastid != 0)) {
			/* 
			 * Release any lingering buffers attached to our local descriptor.
			 * Then copy the name and other business into the cndesc 
			 */
			cat_releasedesc (&cndesc);
			bcopy (&linkdesc, &cndesc, sizeof(linkdesc));	
		}	
		/* If it failed, the linkref code will just use whatever it had in-hand below. */
	}

	if (linkref) {
		int newvnode_flags = 0;
		
        error = hfs_getnewvnode(hfsmp, NULL, NULL, &cndesc, gflags, &cnattr,
								&cnfork, &vp, &newvnode_flags);
		if (error == 0) {
			VTOC(vp)->c_flag |= C_HARDLINK;
//			vnode_setmultipath(vp);
		}
	} else {
		int newvnode_flags = 0;

		void *buf = hfs_malloc(MAXPATHLEN);

		/* Supply hfs_getnewvnode with a component name. */
		struct componentname cn = {
			.cn_nameiop = LOOKUP,
			.cn_flags	= ISLASTCN,
//			.cn_pnlen	= MAXPATHLEN,
			.cn_namelen = cndesc.cd_namelen,
			.cn_pnbuf	= buf,
			.cn_nameptr = buf
		};

		bcopy(cndesc.cd_nameptr, cn.cn_nameptr, cndesc.cd_namelen + 1);

        error = hfs_getnewvnode(hfsmp, NULLVP, &cn, &cndesc, gflags, &cnattr,
								&cnfork, &vp, &newvnode_flags);

		if (error == 0 && (VTOC(vp)->c_flag & C_HARDLINK)) {
			hfs_savelinkorigin(VTOC(vp), cndesc.cd_parentcnid);
		}

		hfs_free(buf, MAXPATHLEN);
	}
	cat_releasedesc(&cndesc);

	*vpp = vp;
    if (vp && ((gflags.lkflags & LK_TYPE_MASK) == 0)) {
		hfs_unlock(VTOC(vp));
	}
	trace_return (error);
}


/*
 * Flush out all the files in a filesystem.
 */
static int
#if QUOTA
hfs_flushfiles(struct mount *mp, int flags, struct proc *p)
#else
hfs_flushfiles(struct mount *mp, int flags, __unused struct thread *td)
#endif /* QUOTA */
{
	struct hfsmount *hfsmp;
	struct vnode *skipvp = NULLVP;
	int error;
	int accounted_root_usecounts;
#if QUOTA
	int i;
#endif

	hfsmp = VFSTOHFS(mp);

	accounted_root_usecounts = 0;
#if QUOTA
	/*
	 * The open quota files have an indirect reference on
	 * the root directory vnode.  We must account for this
	 * extra reference when doing the intial vflush.
	 */
	if (((unsigned int)mp->mnt_flag) & MNT_QUOTA) {
		/* Find out how many quota files we have open. */
		for (i = 0; i < MAXQUOTAS; i++) {
			if (hfsmp->hfs_qfiles[i].qf_vp != NULLVP)
				++accounted_root_usecounts;
		}
	}
#endif /* QUOTA */
    
    // FIXME: check this in kernel...
//	if (accounted_root_usecounts > 0) {
//		/* Obtain the root vnode so we can skip over it. */
//		skipvp = hfs_chash_getvnode(hfsmp, kHFSRootFolderID, 0, 0, 0);
//	}

	error = vflush(mp, (int)accounted_root_usecounts > 0, SKIPSYSTEM | flags, td);
	if (error != 0)
		trace_return (error);

	error = vflush(mp, (int)accounted_root_usecounts > 0, SKIPSYSTEM | flags, td);

	if (skipvp) {
		/*
		 * See if there are additional references on the
		 * root vp besides the ones obtained from the open
		 * quota files and CoreStorage.
		 */
		if ((error == 0) &&
		    (vnode_isinuse(skipvp,  accounted_root_usecounts))) {
			error = EBUSY;  /* root directory is still open */
		}
		hfs_unlock(VTOC(skipvp));
		/* release the iocount from the hfs_chash_getvnode call above. */
		vrele(skipvp);
	}
	if (error && (flags & FORCECLOSE) == 0)
		trace_return (error);

#if QUOTA
	if (((unsigned int)mp->mnt_flag) & MNT_QUOTA) {
		for (i = 0; i < MAXQUOTAS; i++) {
			if (hfsmp->hfs_qfiles[i].qf_vp == NULLVP)
				continue;
			hfs_quotaoff(p, mp, i);
		}
	}
#endif /* QUOTA */

	if (skipvp) {
        
		error = vflush(mp, 0, SKIPSYSTEM | flags, td);
	}

	trace_return (error);
}

/*
 * Update volume encoding bitmap (HFS Plus only)
 * 
 * Mark a legacy text encoding as in-use (as needed)
 * in the volume header of this HFS+ filesystem.
 */
void
hfs_setencodingbits(struct hfsmount *hfsmp, u_int32_t encoding)
{
#define  kIndexMacUkrainian	48  /* MacUkrainian encoding is 152 */
#define  kIndexMacFarsi		49  /* MacFarsi encoding is 140 */

	u_int32_t	index;

	switch (encoding) {
	case kTextEncodingMacUkrainian:
		index = kIndexMacUkrainian;
		break;
	case kTextEncodingMacFarsi:
		index = kIndexMacFarsi;
		break;
	default:
		index = encoding;
		break;
	}

	/* Only mark the encoding as in-use if it wasn't already set */
	if (index < 64 && (hfsmp->encodingsBitmap & (u_int64_t)(1ULL << index)) == 0) {
		hfs_lock_mount (hfsmp);
		hfsmp->encodingsBitmap |= (u_int64_t)(1ULL << index);
		MarkVCBDirty(hfsmp);
		hfs_unlock_mount(hfsmp);
	}
}

/*
 * Update volume stats
 *
 * On journal volumes this will cause a volume header flush
 */
int
hfs_volupdate(struct hfsmount *hfsmp, enum volop op, int inroot)
{
	struct timeval tv;

	microtime(&tv);

	hfs_lock_mount (hfsmp);

	MarkVCBDirty(hfsmp);
	hfsmp->hfs_mtime = tv.tv_sec;

	switch (op) {
	case VOL_UPDATE:
		break;
	case VOL_MKDIR:
		if (hfsmp->hfs_dircount != 0xFFFFFFFF)
			++hfsmp->hfs_dircount;
		if (inroot && hfsmp->vcbNmRtDirs != 0xFFFF)
			++hfsmp->vcbNmRtDirs;
		break;
	case VOL_RMDIR:
		if (hfsmp->hfs_dircount != 0)
			--hfsmp->hfs_dircount;
		if (inroot && hfsmp->vcbNmRtDirs != 0xFFFF)
			--hfsmp->vcbNmRtDirs;
		break;
	case VOL_MKFILE:
		if (hfsmp->hfs_filecount != 0xFFFFFFFF)
			++hfsmp->hfs_filecount;
		if (inroot && hfsmp->vcbNmFls != 0xFFFF)
			++hfsmp->vcbNmFls;
		break;
	case VOL_RMFILE:
		if (hfsmp->hfs_filecount != 0)
			--hfsmp->hfs_filecount;
		if (inroot && hfsmp->vcbNmFls != 0xFFFF)
			--hfsmp->vcbNmFls;
		break;
	}

	hfs_unlock_mount (hfsmp);

	if (hfsmp->jnl) {
		hfs_flushvolumeheader(hfsmp, 0);
	}

	return (0);
}


#if CONFIG_HFS_STD
/* HFS Standard MDB flush */
static int
hfs_flushMDB(struct hfsmount *hfsmp, int waitfor, int altflush)
{
	ExtendedVCB *vcb = HFSTOVCB(hfsmp);
	struct filefork *fp;
	HFSMasterDirectoryBlock	*mdb;
	struct buf *bp = NULL;
	int retval;
	int sector_size;
	ByteCount namelen;

	sector_size = hfsmp->hfs_logical_block_size;
	retval = (int)buf_bread(hfsmp->hfs_devvp, (daddr_t)HFS_PRI_SECTOR(sector_size), sector_size, NOCRED, &bp);
	if (retval) {
		if (bp)
			brelse(bp);
		return retval;
	}

	hfs_lock_mount (hfsmp);

	mdb = (HFSMasterDirectoryBlock *)(bp->b_data + HFS_PRI_OFFSET(sector_size));
    
	mdb->drCrDate	= SWAP_BE32 (UTCToLocal(to_hfs_time(vcb->hfs_itime)));
	mdb->drLsMod	= SWAP_BE32 (UTCToLocal(to_hfs_time(vcb->vcbLsMod)));
	mdb->drAtrb	= SWAP_BE16 (vcb->vcbAtrb);
	mdb->drNmFls	= SWAP_BE16 (vcb->vcbNmFls);
	mdb->drAllocPtr	= SWAP_BE16 (vcb->nextAllocation);
	mdb->drClpSiz	= SWAP_BE32 (vcb->vcbClpSiz);
	mdb->drNxtCNID	= SWAP_BE32 (vcb->vcbNxtCNID);
	mdb->drFreeBks	= SWAP_BE16 (vcb->freeBlocks);

	namelen = strlen((char *)vcb->vcbVN);
	retval = utf8_to_hfs(vcb, namelen, vcb->vcbVN, mdb->drVN);
	/* Retry with MacRoman in case that's how it was exported. */
	if (retval)
		retval = utf8_to_mac_roman(namelen, vcb->vcbVN, mdb->drVN);
	
	mdb->drVolBkUp	= SWAP_BE32 (UTCToLocal(to_hfs_time(vcb->vcbVolBkUp)));
	mdb->drWrCnt	= SWAP_BE32 (vcb->vcbWrCnt);
	mdb->drNmRtDirs	= SWAP_BE16 (vcb->vcbNmRtDirs);
	mdb->drFilCnt	= SWAP_BE32 (vcb->vcbFilCnt);
	mdb->drDirCnt	= SWAP_BE32 (vcb->vcbDirCnt);
	
	bcopy(vcb->vcbFndrInfo, mdb->drFndrInfo, sizeof(mdb->drFndrInfo));

	fp = VTOF(vcb->extentsRefNum);
	mdb->drXTExtRec[0].startBlock = SWAP_BE16 (fp->ff_extents[0].startBlock);
	mdb->drXTExtRec[0].blockCount = SWAP_BE16 (fp->ff_extents[0].blockCount);
	mdb->drXTExtRec[1].startBlock = SWAP_BE16 (fp->ff_extents[1].startBlock);
	mdb->drXTExtRec[1].blockCount = SWAP_BE16 (fp->ff_extents[1].blockCount);
	mdb->drXTExtRec[2].startBlock = SWAP_BE16 (fp->ff_extents[2].startBlock);
	mdb->drXTExtRec[2].blockCount = SWAP_BE16 (fp->ff_extents[2].blockCount);
	mdb->drXTFlSize	= SWAP_BE32 (fp->ff_blocks * vcb->blockSize);
	mdb->drXTClpSiz	= SWAP_BE32 (fp->ff_clumpsize);
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	
	fp = VTOF(vcb->catalogRefNum);
	mdb->drCTExtRec[0].startBlock = SWAP_BE16 (fp->ff_extents[0].startBlock);
	mdb->drCTExtRec[0].blockCount = SWAP_BE16 (fp->ff_extents[0].blockCount);
	mdb->drCTExtRec[1].startBlock = SWAP_BE16 (fp->ff_extents[1].startBlock);
	mdb->drCTExtRec[1].blockCount = SWAP_BE16 (fp->ff_extents[1].blockCount);
	mdb->drCTExtRec[2].startBlock = SWAP_BE16 (fp->ff_extents[2].startBlock);
	mdb->drCTExtRec[2].blockCount = SWAP_BE16 (fp->ff_extents[2].blockCount);
	mdb->drCTFlSize	= SWAP_BE32 (fp->ff_blocks * vcb->blockSize);
	mdb->drCTClpSiz	= SWAP_BE32 (fp->ff_clumpsize);
	FTOC(fp)->c_flag &= ~C_MODIFIED;

	MarkVCBClean( vcb );

	hfs_unlock_mount (hfsmp);

	/* If requested, flush out the alternate MDB */
	if (altflush) {
		struct buf *alt_bp = NULL;

		if (bread(hfsmp->hfs_devvp, hfsmp->hfs_partition_avh_sector, sector_size, NOCRED, &alt_bp) == 0) {
			bcopy(mdb, (char *)alt_bp->b_data + HFS_ALT_OFFSET(sector_size), kMDBSize);

			(void) bwrite(alt_bp);
		} else if (alt_bp)
			brelse(alt_bp);
	}

	if (waitfor != MNT_WAIT)
		bawrite(bp);
	else 
		retval = VOP_BWRITE(bp);

	return (retval);
}
#endif

/*
 *  Flush any dirty in-memory mount data to the on-disk
 *  volume header.
 *
 *  Note: the on-disk volume signature is intentionally
 *  not flushed since the on-disk "H+" and "HX" signatures
 *  are always stored in-memory as "H+".
 */
int
hfs_flushvolumeheader(struct hfsmount *hfsmp, 
					  hfs_flush_volume_header_options_t options)
{
	ExtendedVCB *vcb = HFSTOVCB(hfsmp);
	struct filefork *fp;
	HFSPlusVolumeHeader *volumeHeader, *altVH;
	int retval;
	struct buf *bp, *alt_bp;
	int i;
	daddr_t priIDSector;
	bool critical = false;
	u_int16_t  signature;
	u_int16_t  hfsversion;
	daddr_t avh_sector;
	bool altflush = ISSET(options, HFS_FVH_WRITE_ALT);

	if (ISSET(options, HFS_FVH_FLUSH_IF_DIRTY)
		&& !hfs_header_needs_flushing(hfsmp)) {
		return 0;
	}

	if (hfsmp->hfs_flags & HFS_READ_ONLY) {
		return(0);
	}
#if CONFIG_HFS_STD
	if (hfsmp->hfs_flags & HFS_STANDARD) {
		return hfs_flushMDB(hfsmp, ISSET(options, HFS_FVH_WAIT) ? MNT_WAIT : 0, altflush);
	}
#endif
	priIDSector = (daddr_t)((vcb->hfsPlusIOPosOffset / hfsmp->hfs_logical_block_size) +
				  HFS_PRI_SECTOR(hfsmp->hfs_logical_block_size));

	if (hfs_start_transaction(hfsmp) != 0) {
	    return EINVAL;
	}

	bp = NULL;
	alt_bp = NULL;

	retval = (int)bread(hfsmp->hfs_devvp,
			HFS_PHYSBLK_ROUNDDOWN(priIDSector, hfsmp->hfs_log_per_phys),
			hfsmp->hfs_physical_block_size, NOCRED, &bp);
	if (retval) {
		printf("hfs: err %d reading VH blk (vol=%s)\n", retval, vcb->vcbVN);
		goto err_exit;
	}

	volumeHeader = (HFSPlusVolumeHeader *)((char *)bp->b_data + 
			HFS_PRI_OFFSET(hfsmp->hfs_physical_block_size));

	/*
	 * Sanity check what we just read.  If it's bad, try the alternate
	 * instead.
	 */
	signature = SWAP_BE16 (volumeHeader->signature);
	hfsversion   = SWAP_BE16 (volumeHeader->version);
	if ((signature != kHFSPlusSigWord && signature != kHFSXSigWord) ||
	    (hfsversion < kHFSPlusVersion) || (hfsversion > 100) ||
	    (SWAP_BE32 (volumeHeader->blockSize) != vcb->blockSize)) {
		printf("hfs: corrupt VH on %s, sig 0x%04x, ver %d, blksize %d\n",
			       	vcb->vcbVN, signature, hfsversion, 
				SWAP_BE32 (volumeHeader->blockSize));
		hfs_mark_inconsistent(hfsmp, HFS_INCONSISTENCY_DETECTED);

		/* Almost always we read AVH relative to the partition size */
		avh_sector = hfsmp->hfs_partition_avh_sector;

		if (hfsmp->hfs_partition_avh_sector != hfsmp->hfs_fs_avh_sector) {
			/* 
			 * The two altVH offsets do not match --- which means that a smaller file 
			 * system exists in a larger partition.  Verify that we have the correct 
			 * alternate volume header sector as per the current parititon size.  
			 * The GPT device that we are mounted on top could have changed sizes 
			 * without us knowing. 
			 *
			 * We're in a transaction, so it's safe to modify the partition_avh_sector 
			 * field if necessary.
			 */

			uint64_t sector_count;

			/* Get underlying device block count */
			if ((retval = VNOP_IOCTL(hfsmp->hfs_devvp, HFSTOCP(hfsmp),
                                     DKIOCGETBLOCKCOUNT, (caddr_t)&sector_count, 0))) {
				printf("hfs_flushVH: err %d getting block count (%s) \n", retval, vcb->vcbVN);
				retval = ENXIO;
				goto err_exit;
			}
			
			/* Partition size was changed without our knowledge */
			if (sector_count != (uint64_t)hfsmp->hfs_logical_block_count) {
				hfsmp->hfs_partition_avh_sector = (hfsmp->hfsPlusIOPosOffset / hfsmp->hfs_logical_block_size) + 
					HFS_ALT_SECTOR(hfsmp->hfs_logical_block_size, sector_count);
				/* Note: hfs_fs_avh_sector will remain unchanged */
                printf ("hfs_flushVH: partition size changed, partition_avh_sector=%ld, fs_avh_sector=%ld\n",
						hfsmp->hfs_partition_avh_sector, hfsmp->hfs_fs_avh_sector);

				/* 
				 * We just updated the offset for AVH relative to 
				 * the partition size, so the content of that AVH
				 * will be invalid.  But since we are also maintaining 
				 * a valid AVH relative to the file system size, we 
				 * can read it since primary VH and partition AVH 
				 * are not valid. 
				 */
				avh_sector = hfsmp->hfs_fs_avh_sector;
			}
		}

        printf ("hfs: trying alternate (for %s) avh_sector=%ld\n",
				(avh_sector == hfsmp->hfs_fs_avh_sector) ? "file system" : "partition", avh_sector);

		if (avh_sector) {
			retval = bread(hfsmp->hfs_devvp,
			    HFS_PHYSBLK_ROUNDDOWN(avh_sector, hfsmp->hfs_log_per_phys),
			    hfsmp->hfs_physical_block_size, NOCRED, &alt_bp);
			if (retval) {
				printf("hfs: err %d reading alternate VH (%s)\n", retval, vcb->vcbVN);
				goto err_exit;
			}
			
            altVH = (HFSPlusVolumeHeader *)((char *)alt_bp->b_data +
				HFS_ALT_OFFSET(hfsmp->hfs_physical_block_size));
			signature = SWAP_BE16(altVH->signature);
			hfsversion = SWAP_BE16(altVH->version);
			
			if ((signature != kHFSPlusSigWord && signature != kHFSXSigWord) ||
			    (hfsversion < kHFSPlusVersion) || (kHFSPlusVersion > 100) ||
			    (SWAP_BE32(altVH->blockSize) != vcb->blockSize)) {
				printf("hfs: corrupt alternate VH on %s, sig 0x%04x, ver %d, blksize %d\n",
				    vcb->vcbVN, signature, hfsversion,
				    SWAP_BE32(altVH->blockSize));
				retval = EIO;
				goto err_exit;
			}
			
			/* The alternate is plausible, so use it. */
			bcopy(altVH, volumeHeader, kMDBSize);
			brelse(alt_bp);
			alt_bp = NULL;
		} else {
			/* No alternate VH, nothing more we can do. */
			retval = EIO;
			goto err_exit;
		}
	}

	if (hfsmp->jnl) {
		journal_modify_block_start(hfsmp->jnl, bp);
	}

	/*
	 * For embedded HFS+ volumes, update create date if it changed
	 * (ie from a setattrlist call)
	 */
	if ((vcb->hfsPlusIOPosOffset != 0) &&
	    (SWAP_BE32 (volumeHeader->createDate) != vcb->localCreateDate)) {
		struct buf *bp2;
		HFSMasterDirectoryBlock	*mdb;

		retval = (int)bread(hfsmp->hfs_devvp,
				HFS_PHYSBLK_ROUNDDOWN(HFS_PRI_SECTOR(hfsmp->hfs_logical_block_size), hfsmp->hfs_log_per_phys),
				hfsmp->hfs_physical_block_size, NOCRED, &bp2);
		if (retval) {
			if (bp2)
				brelse(bp2);
			retval = 0;
		} else {
			mdb = (HFSMasterDirectoryBlock *)((bp2->b_data) +
				HFS_PRI_OFFSET(hfsmp->hfs_physical_block_size));

			if ( SWAP_BE32 (mdb->drCrDate) != vcb->localCreateDate )
			  {
				if (hfsmp->jnl) {
				    journal_modify_block_start(hfsmp->jnl, bp2);
				}

				mdb->drCrDate = SWAP_BE32 (vcb->localCreateDate);	/* pick up the new create date */

				if (hfsmp->jnl) {
					journal_modify_block_end(hfsmp->jnl, bp2, NULL, NULL);
				} else {
					(void) bwrite(bp2);		/* write out the changes */
				}
			  }
			else
			  {
				brelse(bp2);						/* just release it */
			  }
		  }	
	}

	hfs_lock_mount (hfsmp);

	/* Note: only update the lower 16 bits worth of attributes */
	volumeHeader->attributes       = SWAP_BE32 (vcb->vcbAtrb);
	volumeHeader->journalInfoBlock = SWAP_BE32 (vcb->vcbJinfoBlock);
	if (hfsmp->jnl) {
		volumeHeader->lastMountedVersion = SWAP_BE32 (kHFSJMountVersion);
	} else {
		volumeHeader->lastMountedVersion = SWAP_BE32 (kHFSPlusMountVersion);
	}
	volumeHeader->createDate	= SWAP_BE32 (vcb->localCreateDate);  /* volume create date is in local time */
	volumeHeader->modifyDate	= SWAP_BE32 (to_hfs_time(vcb->vcbLsMod));
	volumeHeader->backupDate	= SWAP_BE32 (to_hfs_time(vcb->vcbVolBkUp));
	volumeHeader->fileCount		= SWAP_BE32 (vcb->vcbFilCnt);
	volumeHeader->folderCount	= SWAP_BE32 (vcb->vcbDirCnt);
	volumeHeader->totalBlocks	= SWAP_BE32 (vcb->totalBlocks);
	volumeHeader->freeBlocks	= SWAP_BE32 (vcb->freeBlocks + vcb->reclaimBlocks);
	volumeHeader->nextAllocation	= SWAP_BE32 (vcb->nextAllocation);
	volumeHeader->rsrcClumpSize	= SWAP_BE32 (vcb->vcbClpSiz);
	volumeHeader->dataClumpSize	= SWAP_BE32 (vcb->vcbClpSiz);
	volumeHeader->nextCatalogID	= SWAP_BE32 (vcb->vcbNxtCNID);
	volumeHeader->writeCount	= SWAP_BE32 (vcb->vcbWrCnt);
	volumeHeader->encodingsBitmap	= SWAP_BE64 (vcb->encodingsBitmap);

	if (bcmp(vcb->vcbFndrInfo, volumeHeader->finderInfo, sizeof(volumeHeader->finderInfo)) != 0) {
		bcopy(vcb->vcbFndrInfo, volumeHeader->finderInfo, sizeof(volumeHeader->finderInfo));
		critical = true;
	}

	if (!altflush && !ISSET(options, HFS_FVH_FLUSH_IF_DIRTY)) {
		goto done;
	}

	/* Sync Extents over-flow file meta data */
	fp = VTOF(vcb->extentsRefNum);
	if (FTOC(fp)->c_flag & C_MODIFIED) {
		for (i = 0; i < kHFSPlusExtentDensity; i++) {
			volumeHeader->extentsFile.extents[i].startBlock	=
				SWAP_BE32 (fp->ff_extents[i].startBlock);
			volumeHeader->extentsFile.extents[i].blockCount	=
				SWAP_BE32 (fp->ff_extents[i].blockCount);
		}
		volumeHeader->extentsFile.logicalSize = SWAP_BE64 (fp->ff_size);
		volumeHeader->extentsFile.totalBlocks = SWAP_BE32 (fp->ff_blocks);
		volumeHeader->extentsFile.clumpSize   = SWAP_BE32 (fp->ff_clumpsize);
		FTOC(fp)->c_flag &= ~C_MODIFIED;
		altflush = true;
	}

	/* Sync Catalog file meta data */
	fp = VTOF(vcb->catalogRefNum);
	if (FTOC(fp)->c_flag & C_MODIFIED) {
		for (i = 0; i < kHFSPlusExtentDensity; i++) {
			volumeHeader->catalogFile.extents[i].startBlock	=
				SWAP_BE32 (fp->ff_extents[i].startBlock);
			volumeHeader->catalogFile.extents[i].blockCount	=
				SWAP_BE32 (fp->ff_extents[i].blockCount);
		}
		volumeHeader->catalogFile.logicalSize = SWAP_BE64 (fp->ff_size);
		volumeHeader->catalogFile.totalBlocks = SWAP_BE32 (fp->ff_blocks);
		volumeHeader->catalogFile.clumpSize   = SWAP_BE32 (fp->ff_clumpsize);
		FTOC(fp)->c_flag &= ~C_MODIFIED;
		altflush = true;
	}

	/* Sync Allocation file meta data */
	fp = VTOF(vcb->allocationsRefNum);
	if (FTOC(fp)->c_flag & C_MODIFIED) {
		for (i = 0; i < kHFSPlusExtentDensity; i++) {
			volumeHeader->allocationFile.extents[i].startBlock =
				SWAP_BE32 (fp->ff_extents[i].startBlock);
			volumeHeader->allocationFile.extents[i].blockCount =
				SWAP_BE32 (fp->ff_extents[i].blockCount);
		}
		volumeHeader->allocationFile.logicalSize = SWAP_BE64 (fp->ff_size);
		volumeHeader->allocationFile.totalBlocks = SWAP_BE32 (fp->ff_blocks);
		volumeHeader->allocationFile.clumpSize   = SWAP_BE32 (fp->ff_clumpsize);
		FTOC(fp)->c_flag &= ~C_MODIFIED;
		altflush = true;
	}

	/* Sync Attribute file meta data */
	if (hfsmp->hfs_attribute_vp) {
		fp = VTOF(hfsmp->hfs_attribute_vp);
		for (i = 0; i < kHFSPlusExtentDensity; i++) {
			volumeHeader->attributesFile.extents[i].startBlock =
				SWAP_BE32 (fp->ff_extents[i].startBlock);
			volumeHeader->attributesFile.extents[i].blockCount =
				SWAP_BE32 (fp->ff_extents[i].blockCount);
		}
		if (ISSET(FTOC(fp)->c_flag, C_MODIFIED)) {
			FTOC(fp)->c_flag &= ~C_MODIFIED;
			altflush = true;
		}
		volumeHeader->attributesFile.logicalSize = SWAP_BE64 (fp->ff_size);
		volumeHeader->attributesFile.totalBlocks = SWAP_BE32 (fp->ff_blocks);
		volumeHeader->attributesFile.clumpSize   = SWAP_BE32 (fp->ff_clumpsize);
	}

	/* Sync Startup file meta data */
	if (hfsmp->hfs_startup_vp) {
		fp = VTOF(hfsmp->hfs_startup_vp);
		if (FTOC(fp)->c_flag & C_MODIFIED) {
			for (i = 0; i < kHFSPlusExtentDensity; i++) {
				volumeHeader->startupFile.extents[i].startBlock =
					SWAP_BE32 (fp->ff_extents[i].startBlock);
				volumeHeader->startupFile.extents[i].blockCount =
					SWAP_BE32 (fp->ff_extents[i].blockCount);
			}
			volumeHeader->startupFile.logicalSize = SWAP_BE64 (fp->ff_size);
			volumeHeader->startupFile.totalBlocks = SWAP_BE32 (fp->ff_blocks);
			volumeHeader->startupFile.clumpSize   = SWAP_BE32 (fp->ff_clumpsize);
			FTOC(fp)->c_flag &= ~C_MODIFIED;
			altflush = true;
		}
	}

	if (altflush)
		critical = true;
 
done:
	MarkVCBClean(hfsmp);
	hfs_unlock_mount (hfsmp);

	/* If requested, flush out the alternate volume header */
	if (altflush) {
		/* 
		 * The two altVH offsets do not match --- which means that a smaller file 
		 * system exists in a larger partition.  Verify that we have the correct 
		 * alternate volume header sector as per the current parititon size.  
		 * The GPT device that we are mounted on top could have changed sizes 
		 * without us knowning. 
		 *
		 * We're in a transaction, so it's safe to modify the partition_avh_sector 
		 * field if necessary.
		 */
		if (hfsmp->hfs_partition_avh_sector != hfsmp->hfs_fs_avh_sector) {
			uint64_t sector_count;

			/* Get underlying device block count */
			if ((retval = VNOP_IOCTL(hfsmp->hfs_devvp, HFSTOCP(hfsmp), DKIOCGETBLOCKCOUNT,
                                     (caddr_t)&sector_count, 0))) {
				printf("hfs_flushVH: err %d getting block count (%s) \n", retval, vcb->vcbVN);
				retval = ENXIO;
				goto err_exit;
			}
			
			/* Partition size was changed without our knowledge */
			if (sector_count != (uint64_t)hfsmp->hfs_logical_block_count) {
				hfsmp->hfs_partition_avh_sector = (hfsmp->hfsPlusIOPosOffset / hfsmp->hfs_logical_block_size) + 
					HFS_ALT_SECTOR(hfsmp->hfs_logical_block_size, sector_count);
				/* Note: hfs_fs_avh_sector will remain unchanged */
                printf ("hfs_flushVH: altflush: partition size changed, partition_avh_sector=%ld, fs_avh_sector=%ld\n",
						hfsmp->hfs_partition_avh_sector, hfsmp->hfs_fs_avh_sector);
			}
		}

		/*
		 * First see if we need to write I/O to the "secondary" AVH 
		 * located at FS Size - 1024 bytes, because this one will 
		 * always go into the journal.  We put this AVH into the journal
		 * because even if the filesystem size has shrunk, this LBA should be 
		 * reachable after the partition-size modification has occurred.  
		 * The one where we need to be careful is partitionsize-1024, since the
		 * partition size should hopefully shrink. 
		 *
		 * Most of the time this block will not execute.
		 */
		if ((hfsmp->hfs_fs_avh_sector) && 
				(hfsmp->hfs_partition_avh_sector != hfsmp->hfs_fs_avh_sector)) {
			if (bread(hfsmp->hfs_devvp,
						HFS_PHYSBLK_ROUNDDOWN(hfsmp->hfs_fs_avh_sector, hfsmp->hfs_log_per_phys),
						hfsmp->hfs_physical_block_size, NOCRED, &alt_bp) == 0) {
				if (hfsmp->jnl) {
					journal_modify_block_start(hfsmp->jnl, alt_bp);
				}

				bcopy(volumeHeader, (char *)(alt_bp->b_data) +
						HFS_ALT_OFFSET(hfsmp->hfs_physical_block_size), 
						kMDBSize);

				if (hfsmp->jnl) {
					journal_modify_block_end(hfsmp->jnl, alt_bp, NULL, NULL);
				} else {
					(void) bwrite(alt_bp);
				}
			} else if (alt_bp) {
				brelse(alt_bp);
			}
		}
		
		/* 
		 * Flush out alternate volume header located at 1024 bytes before
		 * end of the partition as part of journal transaction.  In 
		 * most cases, this will be the only alternate volume header 
		 * that we need to worry about because the file system size is 
		 * same as the partition size, therefore hfs_fs_avh_sector is 
		 * same as hfs_partition_avh_sector. This is the "priority" AVH. 
		 *
		 * However, do not always put this I/O into the journal.  If we skipped the
		 * FS-Size AVH write above, then we will put this I/O into the journal as 
		 * that indicates the two were in sync.  However, if the FS size is
		 * not the same as the partition size, we are tracking two.  We don't
		 * put it in the journal in that case, since if the partition
		 * size changes between uptimes, and we need to replay the journal,
		 * this I/O could generate an EIO if during replay it is now trying 
		 * to access blocks beyond the device EOF.  
		 */
		if (hfsmp->hfs_partition_avh_sector) {
			if (bread(hfsmp->hfs_devvp, 
						HFS_PHYSBLK_ROUNDDOWN(hfsmp->hfs_partition_avh_sector, hfsmp->hfs_log_per_phys),
						hfsmp->hfs_physical_block_size, NOCRED, &alt_bp) == 0) {

				/* only one AVH, put this I/O in the journal. */
				if ((hfsmp->jnl) && (hfsmp->hfs_partition_avh_sector == hfsmp->hfs_fs_avh_sector)) {
					journal_modify_block_start(hfsmp->jnl, alt_bp);
				}

				bcopy(volumeHeader, (char *)(alt_bp->b_data) +
						HFS_ALT_OFFSET(hfsmp->hfs_physical_block_size), 
						kMDBSize);

				/* If journaled and we only have one AVH to track */
				if ((hfsmp->jnl) && (hfsmp->hfs_partition_avh_sector == hfsmp->hfs_fs_avh_sector)) {
					journal_modify_block_end (hfsmp->jnl, alt_bp, NULL, NULL);
				} else {
					/* 
					 * If we don't have a journal or there are two AVH's at the
					 * moment, then this one doesn't go in the journal.  Note that 
					 * this one may generate I/O errors, since the partition
					 * can be resized behind our backs at any moment and this I/O 
					 * may now appear to be beyond the device EOF.
					 */
					(void) bwrite(alt_bp);
					hfs_flush(hfsmp, HFS_FLUSH_CACHE);
				}		
			} else if (alt_bp) {
				brelse(alt_bp);
			}
		}
	}

	/* Finish modifying the block for the primary VH */
	if (hfsmp->jnl) {
		journal_modify_block_end(hfsmp->jnl, bp, NULL, NULL);
	} else {
		if (!ISSET(options, HFS_FVH_WAIT)) {
			bawrite(bp);
		} else {
			retval = bwrite(bp);
			/* When critical data changes, flush the device cache */
			if (critical && (retval == 0)) {
				hfs_flush(hfsmp, HFS_FLUSH_CACHE);
			}
		}
	}
	hfs_end_transaction(hfsmp);
 
	return (retval);

err_exit:
	if (alt_bp)
		brelse(alt_bp);
	if (bp)
		brelse(bp);
	hfs_end_transaction(hfsmp);
	return retval;
}


/*
 * Creates a UUID from a unique "name" in the HFS UUID Name space.
 * See version 3 UUID.
 */
void
hfs_getvoluuid(struct hfsmount *hfsmp, uuid_t result_uuid)
{

	if (uuid_is_null(hfsmp->hfs_full_uuid)) {
		uuid_t result;

		MD5_CTX  md5c;
		uint8_t  rawUUID[8];

		((uint32_t *)rawUUID)[0] = hfsmp->vcbFndrInfo[6];
		((uint32_t *)rawUUID)[1] = hfsmp->vcbFndrInfo[7];

		MD5Init( &md5c );
		MD5Update( &md5c, HFS_UUID_NAMESPACE_ID, sizeof( uuid_t ) );
		MD5Update( &md5c, rawUUID, sizeof (rawUUID) );
		MD5Final( result, &md5c );

		result[6] = 0x30 | ( result[6] & 0x0F );
		result[8] = 0x80 | ( result[8] & 0x3F );
	
		uuid_copy(hfsmp->hfs_full_uuid, result);
	}
	uuid_copy (result_uuid, hfsmp->hfs_full_uuid);

}

/*
 * Get file system attributes.
 */
// TODO: CRITICAL: if this is deleted or replace with hfs_statfs, make sure statfs::f_fsid is handled properly in hfs_mount()/hfs_journal_replay()
// to prevent mount/unmount bugs.
static int
hfs_vfs_getattr(struct mount *mp, struct statfs *fsap)
{

	ExtendedVCB *vcb = VFSTOVCB(mp);
	struct hfsmount *hfsmp = VFSTOHFS(mp);

	__unused int searchfs_on = 0;
	__unused int exchangedata_on = 1;

#if CONFIG_SEARCHFS
	searchfs_on = 1;
#endif

#if CONFIG_PROTECT
	if (cp_fs_protected(mp)) {
		exchangedata_on = 0;
	}
#endif

	/*
	 * Some of these attributes can be expensive to query if we're
	 * backed by a disk image; hfs_freeblks() has to ask the backing
	 * store, and this might involve a trip to a network file server.
	 * Only ask for them if the caller really wants them.  Preserve old
	 * behavior for file systems not backed by a disk image.
	 */
#if HFS_SPARSE_DEV
	__unused const int diskimage = (hfsmp->hfs_backingvp != NULL);
#else
	const int diskimage = 0;
#endif

	fsap->f_iosize = (size_t) mp->mnt_iosize_max;
	fsap->f_blocks = (u_int64_t)hfsmp->totalBlocks;
    fsap->f_bfree = ((u_int64_t)hfs_freeblks(hfsmp, 0));
    fsap->f_bavail = ((u_int64_t)hfs_freeblks(hfsmp, 1));
	fsap->f_bsize = ((u_int32_t)vcb->blockSize);
	fsap->f_files = ((u_int64_t)HFS_MAX_FILES);
	fsap->f_ffree = ((u_int64_t)hfs_free_cnids(hfsmp));

	fsap->f_fsid.val[0] = (uint32_t) dev2udev(hfsmp->hfs_raw_dev);
	fsap->f_fsid.val[1] = (mp->mnt_vfc->vfc_typenum);
    fsap->f_namemax = kHFSPlusMaxFileNameChars;  /* 255 */

	return (0);
}

/*
 * Perform a volume rename.  Requires the FS' root vp.
 */
__unused
static int
hfs_rename_volume(struct vnode *vp, const char *name, struct proc* p)
{
	ExtendedVCB *vcb = VTOVCB(vp);
	struct cnode *cp = VTOC(vp);
	struct hfsmount *hfsmp = VTOHFS(vp);
	struct cat_desc to_desc;
	struct cat_desc todir_desc;
	struct cat_desc new_desc;
	cat_cookie_t cookie;
	int lockflags;
	int error = 0;
	char converted_volname[256];
	size_t volname_length = 0;
	size_t conv_volname_length = 0;
	

	/*
	 * Ignore attempts to rename a volume to a zero-length name.
	 */
	if (name[0] == 0)
		return(0);

	bzero(&to_desc, sizeof(to_desc));
	bzero(&todir_desc, sizeof(todir_desc));
	bzero(&new_desc, sizeof(new_desc));
	bzero(&cookie, sizeof(cookie));

	todir_desc.cd_parentcnid = kHFSRootParentID;
	todir_desc.cd_cnid = kHFSRootFolderID;
	todir_desc.cd_flags = CD_ISDIR;

	to_desc.cd_nameptr = (const u_int8_t *)name;
	to_desc.cd_namelen = strlen(name);
	to_desc.cd_parentcnid = kHFSRootParentID;
	to_desc.cd_cnid = cp->c_cnid;
	to_desc.cd_flags = CD_ISDIR;

	if ((error = hfs_lock(cp, HFS_EXCLUSIVE_LOCK, HFS_LOCK_DEFAULT)) == 0) {
		if ((error = hfs_start_transaction(hfsmp)) == 0) {
			if ((error = cat_preflight(hfsmp, CAT_RENAME, &cookie, p)) == 0) {
				lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_EXCLUSIVE_LOCK);

				error = cat_rename(hfsmp, &cp->c_desc, &todir_desc, &to_desc, &new_desc);

				/*
				 * If successful, update the name in the VCB, ensure it's terminated.
				 */
				if (error == 0) {
					strlcpy((char *)vcb->vcbVN, name, sizeof(vcb->vcbVN));

					volname_length = strlen ((const char*)vcb->vcbVN);
					/* Send the volume name down to CoreStorage if necessary */	
					error = utf8_normalizestr(vcb->vcbVN, volname_length, (u_int8_t*)converted_volname, &conv_volname_length, 256, UTF_PRECOMPOSED);
//					if (error == 0) {
//						(void) VNOP_IOCTL (hfsmp->hfs_devvp, _DKIOCCSSETLVNAME, converted_volname, 0, cred, curthread);
//					}
					error = 0;
				}
				
				hfs_systemfile_unlock(hfsmp, lockflags);
				cat_postflight(hfsmp, &cookie, p);
			
				if (error)
					MarkVCBDirty(vcb);
				(void) hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT);
			}
			hfs_end_transaction(hfsmp);
		}			
		if (!error) {
			/* Release old allocated name buffer */
			if (cp->c_desc.cd_flags & CD_HASBUF) {
				const char *tmp_name = (const char *)cp->c_desc.cd_nameptr;
		
				cp->c_desc.cd_nameptr = 0;
				cp->c_desc.cd_namelen = 0;
				cp->c_desc.cd_flags &= ~CD_HASBUF;
				vfs_removename(tmp_name);
			}			
			/* Update cnode's catalog descriptor */
			replace_desc(cp, &new_desc);
			vcb->volumeNameEncodingHint = new_desc.cd_encoding;
			cp->c_touch_chgtime = TRUE;
		}

		hfs_unlock(cp);
	}
	
	return(error);
}

/*
 * Get file system attributes.
 */
__unused
static int
hfs_vfs_setattr(struct mount *mp, struct statfs *fsap, struct thread *td)
{
	struct ucred* cred = td->td_ucred;
	int error = 0;

	/*
	 * Must be superuser or owner of filesystem to change volume attributes
	 */
    
	if (!priv_check_cred(cred, PRIV_VFS_ADMIN) && (cred->cr_uid != (mp)->mnt_stat.f_owner))
		return(EACCES);

//	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
//		struct vnode* root_vp;
//
//		error = hfs_vfs_root(mp, &root_vp, context);
//		if (error)
//			goto out;
//
//		error = hfs_rename_volume(root_vp, fsap->f_vol_name, td->td_proc);
//		(void) vput(root_vp);
//		if (error)
//			goto out;
//
//		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
//	}
//
//out:
	return error;
}

/* If a runtime corruption is detected, set the volume inconsistent 
 * bit in the volume attributes.  The volume inconsistent bit is a persistent
 * bit which represents that the volume is corrupt and needs repair.  
 * The volume inconsistent bit can be set from the kernel when it detects
 * runtime corruption or from file system repair utilities like fsck_hfs when
 * a repair operation fails.  The bit should be cleared only from file system 
 * verify/repair utility like fsck_hfs when a verify/repair succeeds.
 */
void hfs_mark_inconsistent(struct hfsmount *hfsmp,
								  hfs_inconsistency_reason_t reason)
{
	hfs_lock_mount (hfsmp);
	if ((hfsmp->vcbAtrb & kHFSVolumeInconsistentMask) == 0) {
		hfsmp->vcbAtrb |= kHFSVolumeInconsistentMask;
		MarkVCBDirty(hfsmp);
	}
	if ((hfsmp->hfs_flags & HFS_READ_ONLY)==0) {
		switch (reason) {
		case HFS_INCONSISTENCY_DETECTED:
			printf("hfs_mark_inconsistent: Runtime corruption detected on %s, fsck will be forced on next mount.\n", 
				   hfsmp->vcbVN);
			break;
		case HFS_ROLLBACK_FAILED:
			printf("hfs_mark_inconsistent: Failed to roll back; volume `%s' might be inconsistent; fsck will be forced on next mount.\n", 
				   hfsmp->vcbVN);
			break;
		case HFS_OP_INCOMPLETE:
			printf("hfs_mark_inconsistent: Failed to complete operation; volume `%s' might be inconsistent; fsck will be forced on next mount.\n", 
				   hfsmp->vcbVN);
			break;
		case HFS_FSCK_FORCED:
			printf("hfs_mark_inconsistent: fsck requested for `%s'; fsck will be forced on next mount.\n",
				   hfsmp->vcbVN);
			break;	
		}
	}
	hfs_unlock_mount (hfsmp);
}

/* Replay the journal on the device node provided.  Returns zero if
 * journal replay succeeded or no journal was supposed to be replayed.
 */
__unused
static int hfs_journal_replay(struct vnode* devvp, struct thread *td)
{
	int retval = 0;
	int error = 0;

	/* Replay allowed only on raw devices */
	if (devvp->v_type != VCHR && devvp->v_type != VBLK)
		return EINVAL;

	// FIXME: null cp
	retval = hfs_mountfs(devvp, NULL, NULL, /* journal_replay_only: */ 1, td);
	buf_flushdirtyblks(devvp, TRUE, 0, "hfs_journal_replay");
	
	/* FSYNC the devnode to be sure all data has been flushed */
	error = VOP_FSYNC(devvp, MNT_WAIT, td);
	if (error) {
		retval = error;
	}

	return retval;
}


/* 
 * Cancel the syncer
 */
static void
hfs_syncer_free(struct hfsmount *hfsmp)
{
    if (hfsmp && ISSET(hfsmp->hfs_flags, HFS_RUN_SYNCER)) {
        hfs_syncer_lock(hfsmp);
		CLR(hfsmp->hfs_flags, HFS_RUN_SYNCER);
        hfs_syncer_unlock(hfsmp);

        // Wait for the syncer thread to finish
        if (hfsmp->hfs_syncer_thread) {
			hfs_syncer_wakeup(hfsmp);
			hfs_syncer_lock(hfsmp);
			while (hfsmp->hfs_syncer_thread)
				hfs_syncer_wait(hfsmp, NULL);
			hfs_syncer_unlock(hfsmp);
        }
    }
}

__unused
static int hfs_vfs_ioctl(struct mount *mp, u_long command, caddr_t data,
						 __unused int flags, __unused struct thread *td)
{
	switch (command) {
#if CONFIG_PROTECT
	case FIODEVICELOCKED:
		cp_device_locked_callback(mp, (cp_lock_state_t)data);
		return 0;
#endif
	}
	return ENOTTY;
}

static int hfs_args_parse(struct mount* mp, struct hfs_mount_args *args)
{
    memset(args, 0, sizeof(struct hfs_mount_args));
    
    args->hfs_uid = (uid_t)VNOVAL;
    args->hfs_gid = (gid_t)VNOVAL;
    args->hfs_mask = (mode_t)VNOVAL;
    args->hfs_encoding = (u_int32_t)VNOVAL;
    
    vfs_scanopt(mp->mnt_optnew, "hfs_uid",                  "%d", &args->hfs_uid);
    vfs_scanopt(mp->mnt_optnew, "hfs_gid",                  "%d", &args->hfs_gid);
    vfs_scanopt(mp->mnt_optnew, "hfs_mask",                 "%d", &args->hfs_mask);
    
    vfs_scanopt(mp->mnt_optnew, "hfs_encoding",             "%d", &args->hfs_encoding);
    vfs_scanopt(mp->mnt_optnew, "hfs_timezone",             "%d", &args->hfs_timezone);
    vfs_scanopt(mp->mnt_optnew, "hfs_wrapper",              "%d", &args->hfs_wrapper);
    vfs_scanopt(mp->mnt_optnew, "hfs_noexec",               "%d", &args->hfs_noexec);
    vfs_scanopt(mp->mnt_optnew, "hfs_journal_tbuffer_size", "%d", &args->journal_tbuffer_size);
    vfs_scanopt(mp->mnt_optnew, "hfs_journal_flags",        "%d", &args->journal_flags);
    vfs_scanopt(mp->mnt_optnew, "hfs_journal_disable",      "%d", &args->journal_disable);

    return 0;
}


static int
hfs_cmount(struct mntarg *ma, void *data, uint64_t flags)
{
    struct hfs_mount_args args;
    int error;

    if (data == NULL)
        trace_return (EINVAL);
    
    error = copyin(data, &args, sizeof args);
    if (error)
        trace_return (error);

    ma = mount_argsu(ma, "from", args.fspec, MAXPATHLEN);
    ma = mount_arg(ma, "export", &args.export, sizeof(args.export));
    ma = mount_argf(ma, "uid", "%d", args.hfs_uid);
    ma = mount_argf(ma, "gid", "%d", args.hfs_gid);
    ma = mount_argf(ma, "mask", "%d", args.hfs_mask);

    // FIXME: Copy pasta, but needed for `mount_hfs`
//    ma = mount_argb(ma, args.flags & MSDOSFSMNT_SHORTNAME, "noshortname");
//    ma = mount_argb(ma, args.flags & MSDOSFSMNT_LONGNAME, "nolongname");
//    ma = mount_argb(ma, !(args.flags & MSDOSFSMNT_NOWIN95), "nowin95");
//    ma = mount_argb(ma, args.flags & MSDOSFSMNT_KICONV, "nokiconv");
    
    error = kernel_mount(ma, flags);

    trace_return (error);
}


/*
 * hfs vfs operations.
 */
struct vfsops hfs_vfsops = {
	// .vfs_vptofh    = hfs_vptofh,
	// .vfs_ioctl	  = hfs_vfs_ioctl,
    .vfs_mount              = hfs_mount,
    .vfs_cmount             = hfs_cmount,
    .vfs_unmount            = hfs_unmount,
    .vfs_root               = vfs_cache_root,
    .vfs_cachedroot         = hfs_vfs_root,
    .vfs_quotactl           = hfs_quotactl,
    .vfs_statfs             = hfs_vfs_getattr,
    .vfs_sync               = hfs_sync,
    .vfs_vget               = hfs_vfs_vget,
    .vfs_fhtovp             = hfs_fhtovp,
//    .vfs_checkexp           = ,
    .vfs_init               = hfs_init,
	.vfs_uninit             = hfs_uninit,
    .vfs_extattrctl         = vfs_stdextattrctl,
    .vfs_sysctl             = hfs_sysctl,
//    .vfs_susp_clean         = ,
//    .vfs_reclaim_lowervp    = ,
//    .vfs_unlink_lowervp     = ,
//    .vfs_purge              = ,
};

VFS_SET(hfs_vfsops, hfs, 0);
