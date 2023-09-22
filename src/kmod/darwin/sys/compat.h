//
//  compat.h
//  hfs-freebsd
//
//  Created by jothwolo on 8/22/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#ifndef compat_h
#define compat_h

#include <sys/appleapiopts.h>
#include <sys/cdefs.h>
#include <sys/stdint.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
// disk stuff
#include <sys/ddisk.h>

/* Macros to clear/set/test flags. */
#define    SET(t, f)    (t) |= (f)
#define    CLR(t, f)    (t) &= ~(f)
#define    ISSET(t, f)    ((t) & (f))

// ...
#define KERN_SUCCESS 0

// port this?
#define KERNEL_DEBUG_CONSTANT(...)
#define KDBG(...)
#define KERNEL_DEBUG(...)

// available buf flags
#warning check availability
#define B_SHADOW  B_FS_FLAG1

// sysctl lock flag
#define CTLFLAG_LOCKED CTLFLAG_NEEDGIANT

// timeval stuff
#define    timerclear(tvp)        ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define    timerisset(tvp)        ((tvp)->tv_sec || (tvp)->tv_usec)
#define    timercmp(tvp, uvp, cmp)                    \
    (((tvp)->tv_sec == (uvp)->tv_sec) ?                \
        ((tvp)->tv_usec cmp (uvp)->tv_usec) :            \
        ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define    timeradd(tvp, uvp, vvp)                        \
    do {                                \
        (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;        \
        (vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;    \
        if ((vvp)->tv_usec >= 1000000) {            \
            (vvp)->tv_sec++;                \
            (vvp)->tv_usec -= 1000000;            \
        }                            \
    } while (0)
#define    timersub(tvp, uvp, vvp)                        \
    do {                                \
        (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;        \
        (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;    \
        if ((vvp)->tv_usec < 0) {                \
            (vvp)->tv_sec--;                \
            (vvp)->tv_usec += 1000000;            \
        }                            \
    } while (0)


/*
 * IOCTLs used for filesystem write suspension.
 */
#define    UFSSUSPEND       _IOW('U', 1, fsid_t)
#define    UFSRESUME        _IO('U', 2)

/*
 * Flags to hfs_reload
 */
#define    HFSR_FORCE       0x0001
#define    HFSR_UNSUSPEND   0x0002

// vfs_event flags
#define VQ_NEARLOWDISK      VQ_FLAG0100
#define VQ_VERYLOWDISK      VQ_FLAG0200
#define VQ_UPDATE           VQ_FLAG0400
#define VQ_DESIRED_DISK     VQ_FLAG0800

// xattr stuff
/* See the ATTR_CMN_FNDRINFO section of getattrlist(2) for details on FinderInfo */
#define XATTR_FINDERINFO_NAME       "com.apple.FinderInfo"
#define XATTR_RESOURCEFORK_NAME     "com.apple.ResourceFork"
#define KAUTH_FILESEC_XATTR         "com.apple.system.Security"
#define XATTR_MAXNAMELEN            127 // less than EXTATTR_MAXNAMELEN, so it's okay i think

// buf_iterate stuff
/*
 * flags for buf_flushdirtyblks and buf_iterate
 */
#define BUF_SKIP_NONLOCKED      0x01
#define BUF_SKIP_LOCKED         0x02
#define BUF_SCAN_CLEAN          0x04    /* scan the clean buffers */
#define BUF_SCAN_DIRTY          0x08    /* scan the dirty buffers */
#define BUF_NOTIFY_BUSY         0x10    /* notify the caller about the busy pages during the scan */

#define BUF_RETURNED            0
#define BUF_RETURNED_DONE       1
#define BUF_CLAIMED             2
#define BUF_CLAIMED_DONE        3

// some opaque struct defs.
struct vop_pagein_args;
struct vop_pageout_args;
struct vop_exchange_args;
struct g_consumer;

// some helper functions
bool vnode_isswap(struct vnode *vp);
bool vnode_isinuse(struct vnode *vp, int refcnt);
struct vnode* vnode_getparent(struct vnode *vp);
int vnode_waitforwrites(struct vnode* vp, int output_target, int slpflag, int slptimeout, const char *msg);
int buf_invalidateblks(struct vnode* vp, int flags, int slpflag, int slptimeo);
void buf_flushdirtyblks(struct vnode* vp, int wait, int flags, const char *msg);

int  vnode_isfastdevicecandidate(struct vnode* vp);
void vnode_setfastdevicecandidate(struct vnode* vp);
void vnode_clearfastdevicecandidate(struct vnode* vp);
int  vnode_isautocandidate(struct vnode* vp);
void vnode_setautocandidate(struct vnode* vp);
void vnode_clearautocandidate(struct vnode* vp);

int xattr_protected(const char *);

struct buf * buf_copy(struct buf *bp);
struct buf * buf_shallow_copy(struct buf *bp);
void buf_shallow_free(struct buf *bp);
int buf_shadow(struct buf *bp);
void buf_iterate(struct vnode *vp, int (*callout)(struct buf*, void *), int flags, void *arg);

int cluster_push(struct vnode* vp, int flags);

int VNOP_IOCTL(struct vnode* devvp, struct g_consumer *cp, u_long command, void* data, int fflag);

const char *vfs_addname(const char *name, uint32_t len, uint32_t nc_hash, uint32_t flags);
int   vfs_removename(const char *name);
void  vfs_ioattr(struct mount *mp, struct g_consumer *cp, uint64_t *maxio);

static inline time_t
mach_absolute_time(void)
{
    return ticks;
}

static inline void
absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
    *result = abstime / hz * 1000000000;
}

static inline time_t
nanotime_nsec(void)
{
    struct timespec ts;
    nanotime(&ts);
    return ts.tv_nsec;
}
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor,
                                uint64_t *result);

void delay_for_interval(uint32_t interval, uint32_t scale_factor);

#endif /* compat_h */
