//
//  compat.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/22/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#include "compat.h"

#include <sys/namei.h>
#include <sys/disk.h>
#include <sys/ucred.h>
#include <sys/buf.h>
#include <sys/bio.h>
#include <sys/mount.h>
#include <sys/malloc.h>
#include <sys/fnv_hash.h>
#include <sys/lockmgr.h>
#include <sys/rwlock.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
//#include <vm/vm_pager.h>

#include <geom/geom.h>
#include <geom/geom_int.h>
#include <geom/geom_disk.h>
#include <geom/geom_vfs.h>

// init funciton pototypes
extern void thlocal_init(void);
extern void thlocal_uninit(void);
extern void vfs_names_init(void);
extern void vfs_names_destroy(void);

void darwin_compat_init(void)
{
    thlocal_init();
    vfs_names_init();
}

void darwin_compat_uninit(void)
{
    thlocal_uninit();
    vfs_names_destroy();
    
}

struct vnode*
vnode_getparent(struct vnode *vp)
{
    int error = 0;
    static char *buf = "..";
    struct vnode *parentdp;
    struct componentname cn = (struct componentname){
        .cn_nameiop = LOOKUP,
        .cn_flags    = WANTPARENT | ISDOTDOT,
        .cn_pnbuf    = buf,
        .cn_nameptr  = buf,
        .cn_namelen  = sizeof("..") -1,
    };
    
    error = cache_lookup(vp, &parentdp, &cn, 0, 0);
    if (error == 0 && parentdp != NULL) {
        return parentdp;
    } else {
        return NULL;
    }
}

bool
vnode_isinuse(struct vnode *vp, int refcnt)
{
    return atomic_load_32(&vp->v_usecount) > refcnt;
}

bool
vnode_isswap(struct vnode *vp)
{
    return vp->v_object != NULL ? vp->v_object->type == OBJT_SWAP : false;
}

// Copy of bufobj_wwait()
// this looks dumb. Though it follows HFS's semantics i guess.


int
vnode_waitforwrites(struct vnode* vp, int output_target, int slpflag, int slptimeout, const char *msg)
{
    struct bufobj *bo;
    int error;

    bo = &vp->v_bufobj;
    error = 0;
    
    KASSERT(vp != NULL, ("NULL vp in vnode_waitforwrites"));
    KASSERT(bo != NULL, ("NULL bo in vnode_waitforwrites"));
    
    BO_LOCK(bo);
    
    while (bo->bo_numoutput > output_target) {
        bo->bo_flag |= BO_WWAIT;
        error = msleep(&bo->bo_numoutput, BO_LOCKPTR(bo), slpflag | (PRIBIO + 1), msg, slptimeout);
        if (error)
            break;
    }
    
    BO_UNLOCK(bo);
    return (error);
}

/*
 * We have no way of relaying the wait flag to BO_SYNC. We'll just have to wait all the time.
 * vop_fsync and hfs_reclaim will be affected by this
 */
void
buf_flushdirtyblks(struct vnode* vp, int wait, int flags, const char *msg)
{
    struct bufobj *bo;
    int error, flag;
    
    bo = &vp->v_bufobj;
    flag = V_SAVE;
    error = bufobj_invalbuf(bo, flag, 0, 0);
    
    if (error != 0) {
        printf("[WARNING]: Failed to flush dirty buffers for vnode %p\n", vp);
    }
}

int
buf_invalidateblks(struct vnode* vp, int flags, int slpflag, int slptimeo)
{
    struct bufobj *bo;
    int flag;
    
    bo = &vp->v_bufobj;
    flag = V_SAVE;
    return bufobj_invalbuf(bo, flag, 0, 0);
}

// FIXME: maxio is in kerneldump
// "GEOM::kerneldump"
int
VNOP_IOCTL(struct vnode* devvp, struct g_consumer *cp, u_long command, void* data, int fflag)
{
    int error, len;
    bool cleanup;
    
    // FIXME: change code after refactoring VNOP_IOCTL.
    cleanup = false;
    error = 0;
    
    if (!cp) {
        kdb_backtrace();
        cleanup = true;
        g_topology_lock();
        error = g_vfs_open(devvp, &cp, "hfs", /* rw */1);
        g_topology_unlock();
    }
    
    switch (command) {
        case DKIOCSYNCHRONIZE: {
            struct bio *bp;
            dk_synchronize_t *dsync;
            
            dsync = (dk_synchronize_t *) data;
            bp = malloc(sizeof(*bp), M_TEMP, M_WAITOK | M_ZERO);
            // malloc zeroes out the values...
            
            bp->bio_cmd = BIO_FLUSH;
            bp->bio_data = NULL;
            bp->bio_offset = dsync->offset;
            bp->bio_length = dsync->length;
            bp->bio_done = NULL;
            bp->bio_caller1 = NULL;
            
            // match darwin options to fbsd options
            // so far, only the ordered sync option is used
            if (dsync->options & DK_SYNCHRONIZE_OPTION_BARRIER)
                bp->bio_flags |= BIO_ORDERED;
            
            g_io_request(bp, cp);
            
            free(bp, M_TEMP);
            break;
        }
            
        case DKIOCUNMAP: {
            dk_unmap_t *dk_unmap = data;
            struct bio *bp;
            
            if (dk_unmap->extents == 0){
                error = EINVAL;
                break;
            }
            
            // malloc zeroes out the values...
            bp = malloc(sizeof(*bp), M_TEMP, M_WAITOK | M_ZERO);
            
            bp->bio_cmd = BIO_DELETE;
            bp->bio_data = NULL;
            bp->bio_done = NULL;
            bp->bio_caller1 = NULL;
            
            for (int i = 0; i < dk_unmap->extentsCount; i++) {
                // dk_unmap->options is currently unused in darwin
                bp->bio_offset = dk_unmap->extents[i].offset;
                bp->bio_length = dk_unmap->extents[i].length;
                g_io_request(bp, cp);
            }
            
            free(bp, M_TEMP);
            break;
        }
            
        case DKIOCISVIRTUAL: {
            char ident[32] = {0};
            len = sizeof(ident);
            
            error = g_io_getattr("GEOM::ident", cp, &len, &ident[0]);
            
            // if the disk has "MD-DEV" in it's sc identity variable, it's virtual.
            if (error != 0) {
                printf("WARNING: %s: Could not get ident attribute for disk (error %d)\n",
                       devtoname(devvp->v_rdev), error);
                *(int*)data = 0;
            } else {
                *(int*)data = strncmp(ident, "MD-DEV", sizeof("MD-DEV") - 1) == 0;
            }
            break;
        }
            
        case DKIOCISSOLIDSTATE: {
            int rotation = DISK_RR_UNKNOWN;
            len = sizeof(rotation);
            
            error = g_io_getattr("GEOM::rotation_rate", cp, &len, &rotation);
            
            if (error != 0) {
                printf("WARNING: %s: Could not get ejectable attribute for disk (error %d)\n",
                       devtoname(devvp->v_rdev), error);
                rotation = 2; /* force assumption that there's a spinning disk */
            }
            
            /* if the rotation > 1, then the disk spins */
            *(u_int*)data = rotation == DISK_RR_NON_ROTATING ? 1 : 0;
            break;
        }
        
        case DKIOCGETFEATURES: {
            int candelete = 0, canflush, __unused canspeedup;
            uint64_t *device_features = data;
            
            {
                len = sizeof(candelete);
                g_io_getattr("GEOM::candelete", cp, &len, &candelete);
            }
            {
                // test flush
                struct bio *bp;
                bp = g_alloc_bio();
                bp->bio_cmd = BIO_FLUSH;
                bp->bio_flags = 0;
                bp->bio_done = NULL;
                bp->bio_attribute = NULL;
                bp->bio_offset = cp->provider->mediasize;
                bp->bio_length = 0;
                bp->bio_data = NULL;
                g_io_request(bp, cp);
                canflush = biowait(bp, "hfs_flush_test") != EOPNOTSUPP;
                g_destroy_bio(bp);
            }
#if 0
            {
                // this isn't exactly like DK_FEATURE_PRIORITY, but close enough?
                len = sizeof(int);
                g_io_getattr("GEOM::canspeedup", cp, &len, &canspeedup);
            }
#endif
            *device_features = 0;
            *device_features |= candelete ? DK_FEATURE_UNMAP : 0;
            *device_features |= canflush ? DK_FEATURE_BARRIER : 0;
            
        }
        
        case DKIOCGETMAXBLOCKCOUNTREAD:
        case DKIOCGETMAXBLOCKCOUNTWRITE: {
            struct g_kerneldump gkd;
            
            len = sizeof(gkd);
            error = g_io_getattr("GEOM::kerneldump", cp, &len, &gkd);
            
            *(uint64_t*)data = gkd.di.maxiosize / cp->provider->sectorsize;
            break;
        }
        case DKIOCGETMAXBYTECOUNTREAD:
        case DKIOCGETMAXBYTECOUNTWRITE: {
            struct g_kerneldump gkd;
            
            len = sizeof(gkd);
            error = g_io_getattr("GEOM::kerneldump", cp, &len, &gkd);
            
            *(uint64_t*)data = gkd.di.maxiosize;
            break;
        }
            
        case DKIOCGETBLOCKSIZE: {
            *(u_int*)data = cp->provider->sectorsize;
            break;
        }
            
        case DKIOCGETBLOCKCOUNT: {
            *(uint64_t*)data = cp->provider->mediasize / cp->provider->sectorsize;
            break;
        }
        case _DKIOCCSPINEXTENT:
        case _DKIOCCSUNPINEXTENT:
            break;
        
        // corestorage disks not supported
        case DKIOCCORESTORAGE:
            error = ENOTSUP;
            break;
        
        case DKIOCGETPHYSICALBLOCKSIZE:
            *(uint32_t*)data = cp->provider->sectorsize;
            break;
            
        case DKIOCISWRITABLE:
            *(uint32_t*)data = cp->provider->acw;
            break;
            
        default:
            panic("ERROR: %s: Unsupported VNOP_IOCTL() command %lu\n", devtoname(devvp->v_rdev), command);
            __builtin_unreachable();
    }
    
    if (cleanup){
        g_vfs_close(cp);
    }
    
    return error;
}

int
vnode_isfastdevicecandidate(struct vnode* vp)
{
    return 0;
}

void
vnode_setfastdevicecandidate(struct vnode* vp)
{
    return;
}

void
vnode_clearfastdevicecandidate(struct vnode* vp)
{
    return;
}

int
vnode_isautocandidate(struct vnode* vp)
{
    return 0;
}

void
vnode_setautocandidate(struct vnode* vp)
{
    return;
}

void
vnode_clearautocandidate(struct vnode* vp)
{
    return;
}

int cluster_push(struct vnode* vp, int flags) {
    struct vop_fsync_args args;
    args.a_td = curthread;
    args.a_vp = vp;
    args.a_waitfor = flags & IO_SYNC ? MNT_WAIT : MNT_NOWAIT;
    return vop_stdfsync(&args);
}

/*
 * Return a count of buffers on the "locked" queue.
 */
int
count_lock_queue(void)
{
    return buf_dirty_count_severe();
}

MALLOC_DEFINE(M_COMPAT_BUFFERS, "compat buffers", "darwin compat buffers");

struct buf * buf_copy(struct buf *obp)
{
    struct buf *nbp = NULL;
    
    if (!obp)
        panic("%s: Buffer is null", __func__);
    
    nbp = geteblk((int)obp->b_bufsize, GB_NOWAIT_BD);
    
    pbgetvp(obp->b_vp, nbp); // associate buffer with vnode
    
    nbp->b_flags |= B_SHADOW;
    nbp->b_bufsize = obp->b_bufsize;
    nbp->b_bcount = obp->b_bcount;
    nbp->b_blkno = obp->b_blkno;
    nbp->b_lblkno = obp->b_lblkno;
    
    memcpy(nbp->b_data, obp->b_data, obp->b_bcount);
    
    return nbp;
}

struct buf *
buf_shallow_copy(struct buf *obp)
{
    struct buf *nbp = malloc(sizeof(struct buf), M_COMPAT_BUFFERS, M_ZERO | M_NOWAIT);
    
    if (!obp)
        panic("%s: Buffer is null", __func__);
    
    pbgetvp(obp->b_vp, nbp); // associate buffer with vnode
    
    nbp->b_flags |= B_MANAGED | B_DIRECT | B_NOCACHE | B_SHADOW;
    nbp->b_bufsize = obp->b_bufsize;
    nbp->b_bcount = obp->b_bcount;
    nbp->b_data = (void*)obp->b_data; // shallow...
    nbp->b_blkno = obp->b_blkno;
    nbp->b_lblkno = obp->b_lblkno;
    
    return nbp;
}

void
buf_shallow_free(struct buf *bp)
{
    free(bp, M_COMPAT_BUFFERS);
}

int
buf_shadow(struct buf *bp)
{
    return ISSET(bp->b_flags, B_SHADOW);
}

void
clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result)
{
    *result = ticks + (interval * scale_factor);
}

void
delay_for_interval(uint32_t interval, uint32_t scale_factor)
{
    pause("delay_for_interval", interval*scale_factor);
}

int
xattr_protected(const char *attrname)
{
    return 0;
}

void
buf_iterate(struct vnode *vp, int (*callout)(struct buf*, void *),
            int flags, void *arg)
{
    bool skiplocked = (flags & BUF_SKIP_LOCKED) != 0;
    bool scanlockedonly = (flags & BUF_SKIP_NONLOCKED) != 0;
    __unused bool notifybusy = (flags & BUF_NOTIFY_BUSY) != 0;
    struct bufv *bv = NULL;
    struct bufobj *bo = NULL;
    int ret;
    
    
    bo = &vp->v_bufobj;
    
    BO_LOCK(bo);
    
    if (flags & BUF_SCAN_CLEAN){
        bv = &bo->bo_clean;
        struct buf *bp;
        TAILQ_FOREACH(bp, &bv->bv_hd, b_bobufs){
            bool islocked = BUF_ISLOCKED(bp);
            
            if (!islocked && scanlockedonly)
                continue;
            if (islocked && skiplocked)
                continue;
            
            ret = callout(bp, arg);
            if (ret == BUF_RETURNED_DONE || ret == BUF_CLAIMED_DONE)
                goto out_unlock;
            
        }
    }
    
    if (flags & BUF_SCAN_DIRTY){
        bv = &bo->bo_dirty;
        struct buf *bp;
        TAILQ_FOREACH(bp, &bv->bv_hd, b_bobufs){
            bool islocked = BUF_ISLOCKED(bp);
            
            if (!islocked && scanlockedonly)
                continue;
            if (islocked && skiplocked)
                continue;
            
            ret = callout(bp, arg);
            if (ret == BUF_RETURNED_DONE || ret == BUF_CLAIMED_DONE)
                goto out_unlock;
            
        }
    }
    
out_unlock:
    
    BO_UNLOCK(bo);
}

void
vfs_ioattr(struct mount *mp, struct g_consumer *cp, uint64_t *maxio)
{
    struct g_kerneldump gkd;
    int len, error;
    
    len = sizeof(gkd);
    error = g_io_getattr("GEOM::kerneldump", cp, &len, &gkd);
    
    // if the disk has "MD-DEV" in it's sc identity variable, it's virtual.
    if (error != 0) {
        printf("WARNING: %s: Could not get ident attribute for disk (error %d)\n",
               mp->mnt_stat.f_mntfromname, error);
        *(int*)maxio = 512;
    }
    
    *maxio = gkd.di.maxiosize;
}

void
vnode_update_identity(struct vnode* vp, struct vnode* dvp, const char *name,
                      int name_len, uint32_t name_hashval, int flags)
{
    struct componentname cn = {0};
    cn.cn_pnbuf = (char*) name;
    cn.cn_nameptr = (char*) name;
    cn.cn_namelen = name_len;
    // cn doesn't relaly need other elements init'ed
    
    if ((flags & (VNODE_UPDATE_PURGE | VNODE_UPDATE_PARENT | VNODE_UPDATE_CACHE | VNODE_UPDATE_NAME))) {
        
        cache_purge(vp);
        
        if (flags & VNODE_UPDATE_PURGE){
            return;
        }

        cache_enter(dvp, vp, &cn);
    }
}
