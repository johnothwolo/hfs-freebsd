/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2012 The FreeBSD Foundation
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/ioccom.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/jail.h>
#include <sys/sx.h>

#include    "hfs.h"
#include    "hfs_attrlist.h"
#include    "hfs_endian.h"
#include    "hfs_fsctl.h"
#include    "hfs_quota.h"
#include    "FileMgrInternal.h"
#include    "BTreesInternal.h"
#include    "hfs_cnode.h"
#include    "hfs_dbg.h"

int
hfs_own_mount(const struct mount *mp)
{
    extern struct vfsops hfs_vfsops;
    if (mp->mnt_op == &hfs_vfsops)
        return (1);
    return (0);
}

struct cdevpriv {
    struct mount *mp;
    struct thread *td;
};

static d_open_t hfs_susp_open;
static d_write_t hfs_susp_rdwr;
static d_ioctl_t hfs_susp_ioctl;

static struct cdevsw hfs_susp_cdevsw = {
    .d_version =    D_VERSION,
    .d_open =    hfs_susp_open,
    .d_read =    hfs_susp_rdwr,
    .d_write =    hfs_susp_rdwr,
    .d_ioctl =    hfs_susp_ioctl,
    .d_name =    "hfs_susp",
};

static struct cdev *hfs_susp_dev;
static struct sx hfs_susp_lock;

static int
hfs_susp_suspended(struct mount *mp, struct thread *td)
{
    struct hfsmount *hfsmp;

    sx_assert(&hfs_susp_lock, SA_LOCKED);

    hfsmp = VFSTOHFS(mp);
    if ((hfsmp->hfs_freeze_state & HFS_FROZEN) != 0)
        return (1);
    return (0);
}

static void hfs_thaw_locked(struct hfsmount *hfsmp)
{
    hfsmp->hfs_freezing_proc = NULL;
    hfsmp->hfs_freeze_state = HFS_THAWED;

    wakeup(&hfsmp->hfs_freeze_state);
}

static int
hfs_susp_open(struct cdev *dev __unused, int flags __unused,
    int fmt __unused, struct thread *td __unused)
{

    return (0);
}

static int
hfs_susp_rdwr(struct cdev *dev, struct uio *uio, int ioflag)
{
    int error, i;
    struct vnode *devvp;
    struct cdevpriv *priv;
    struct mount *mp;
    struct hfsmount *hfsmp;
    struct buf *bp;
    void *base;
    size_t len;
    ssize_t cnt;

    sx_slock(&hfs_susp_lock);

    error = devfs_get_cdevpriv((void **)&priv);
    if (error != 0) {
        sx_sunlock(&hfs_susp_lock);
        return (ENXIO);
    }
    
    mp = priv->mp;
    hfsmp = VFSTOHFS(mp);
    devvp = hfsmp->hfs_devvp;

    if (hfs_susp_suspended(mp, priv->td) == 0) {
        sx_sunlock(&hfs_susp_lock);
        return (ENXIO);
    }

    KASSERT(uio->uio_rw == UIO_READ || uio->uio_rw == UIO_WRITE, ("neither UIO_READ or UIO_WRITE"));
    KASSERT(uio->uio_segflg == UIO_USERSPACE, ("uio->uio_segflg != UIO_USERSPACE"));

    cnt = uio->uio_resid;

    for (i = 0; i < uio->uio_iovcnt; i++) {
        while (uio->uio_iov[i].iov_len) {
            base = uio->uio_iov[i].iov_base;
            len = uio->uio_iov[i].iov_len;
            if (len > hfsmp->blockSize)
                len = hfsmp->blockSize;
            if ((uio->uio_offset % hfsmp->blockSize) != 0 ||
                (len % hfsmp->blockSize) != 0) {
                error = EINVAL;
                goto out;
            }
            error = bread(devvp, btodb(uio->uio_offset), len, NOCRED, &bp);
            if (error != 0)
                goto out;
            if (uio->uio_rw == UIO_WRITE) {
                error = copyin(base, bp->b_data, len);
                if (error != 0) {
                    bp->b_flags |= B_INVAL | B_NOCACHE;
                    brelse(bp);
                    goto out;
                }
                error = bwrite(bp);
                if (error != 0)
                    goto out;
            } else {
                error = copyout(bp->b_data, base, len);
                brelse(bp);
                if (error != 0)
                    goto out;
            }
            uio->uio_iov[i].iov_base =
                (char *)uio->uio_iov[i].iov_base + len;
            uio->uio_iov[i].iov_len -= len;
            uio->uio_resid -= len;
            uio->uio_offset += len;
        }
    }

out:
    sx_sunlock(&hfs_susp_lock);

    if (uio->uio_resid < cnt)
        return (0);

    return (error);
}

static int
hfs_susp_suspend(struct mount *mp, struct thread *td)
{
    struct hfsmount *hfsmp;
    int error;

    sx_assert(&hfs_susp_lock, SA_XLOCKED);

    if (!hfs_own_mount(mp))
        return (EINVAL);
    if (hfs_susp_suspended(mp, td))
        return (EBUSY);

    hfsmp = VFSTOHFS(mp);

    /*
     * Make sure the calling thread is permitted to access the mounted
     * device.  The permissions can change after we unlock the vnode;
     * it's harmless.
     */
    vn_lock(hfsmp->hfs_odevvp, LK_EXCLUSIVE | LK_RETRY);
    error = VOP_ACCESS(hfsmp->hfs_odevvp, VREAD | VWRITE,td->td_ucred, td);
    VOP_UNLOCK(hfsmp->hfs_odevvp);
    if (error != 0)
        return (error);
#ifdef MAC
    if (mac_mount_check_stat(td->td_ucred, mp) != 0)
        return (EPERM);
#endif
    
    // First make sure some other process isn't freezing
    hfs_lock_mount(hfsmp);
    while (hfsmp->hfs_freeze_state != HFS_THAWED) {
        if (msleep(&hfsmp->hfs_freeze_state, &hfsmp->hfs_mutex.mtx, PWAIT | PCATCH, "hfs freeze 1", 0) == EINTR) {
            hfs_unlock_mount(hfsmp);
            return EINTR;
        }
    }

    // Stop new syncers from starting
    hfsmp->hfs_freeze_state = HFS_WANT_TO_FREEZE;

    // Now wait for all syncers to finish
    while (hfsmp->hfs_syncers) {
        if (msleep(&hfsmp->hfs_freeze_state, &hfsmp->hfs_mutex.mtx, PWAIT | PCATCH, "hfs freeze 2", 0) == EINTR) {
            hfs_thaw_locked(hfsmp);
            hfs_unlock_mount(hfsmp);
            return EINTR;
        }
    }
    hfs_unlock_mount(hfsmp);

    if ((error = vfs_write_suspend(mp, VS_SKIP_UNMOUNT)) != 0)
        return (error);
    
    // Block everything in hfs_lock_global now
    hfs_lock_mount(hfsmp);
    hfsmp->hfs_freeze_state = HFS_FREEZING;
    hfsmp->hfs_freezing_thread = curthread;
    hfs_unlock_mount(hfsmp);

    /* Take the exclusive lock to flush out anything else that
       might have the global lock at the moment and also so we
       can flush the journal. */
    hfs_lock_global(hfsmp, HFS_EXCLUSIVE_LOCK);
    journal_flush(hfsmp->jnl, JOURNAL_WAIT_FOR_IO);
    hfs_unlock_global(hfsmp);

    // We're done, mark frozen
    hfs_lock_mount(hfsmp);
    hfsmp->hfs_freeze_state  = HFS_FROZEN;
    hfsmp->hfs_freezing_proc = curproc;
    hfs_unlock_mount(hfsmp);
    
    return (0);
}

static void
hfs_susp_unsuspend(struct mount *mp, struct thread *td)
{
    struct hfsmount *hfsmp;

    sx_assert(&hfs_susp_lock, SA_XLOCKED);

    /*
     * XXX: The status is kept per-process; the vfs_write_resume() routine
     *     asserts that the resuming thread is the same one that called
     *     vfs_write_suspend().  The cdevpriv data, however, is attached
     *     to the file descriptor, e.g. is inherited during fork.  Thus,
     *     it's possible that the resuming process will be different from
     *     the one that started the suspension.
     *
     *     Work around by fooling the check in vfs_write_resume().
     */
    mp->mnt_susp_owner = td;

    vfs_write_resume(mp, 0);
    hfsmp = VFSTOHFS(mp);
    hfs_thaw(hfsmp, td->td_proc);
    vfs_unbusy(mp);
}

static void
hfs_susp_dtor(void *data)
{
    struct cdevpriv *priv;
    struct hfsmount *hfsmp;
    struct mount *mp;
    int error;

    sx_xlock(&hfs_susp_lock);

    priv = (struct cdevpriv*)data;
    mp = priv->mp;
    hfsmp = VFSTOHFS(mp);

    if (hfs_susp_suspended(mp, priv->td) == 0) {
        sx_xunlock(&hfs_susp_lock);
        return;
    }

    KASSERT((mp->mnt_kern_flag & MNTK_SUSPEND) != 0,
            ("MNTK_SUSPEND not set"));

    error = hfs_reload(mp, priv->td, HFSR_FORCE | HFSR_UNSUSPEND);
    if (error != 0)
        panic("failed to unsuspend writes on %s", hfsmp->vcbVN);

    hfs_susp_unsuspend(mp, priv->td);
    sx_xunlock(&hfs_susp_lock);
    free(priv, M_TEMP);
}

static int
hfs_susp_ioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flags,
    struct thread *td)
{
    struct cdevpriv* priv;
    struct mount *mp;
    fsid_t *fsidp;
    int error;

    /*
     * No suspend inside the jail.  Allowing it would require making
     * sure that e.g. the devfs ruleset for that jail permits access
     * to the devvp.
     */
    if (jailed(td->td_ucred))
        return (EPERM);

    sx_xlock(&hfs_susp_lock);

    switch (cmd) {
    case HFSIOC_SUSPEND:
        fsidp = (fsid_t *)addr;
        mp = vfs_getvfs(fsidp);
        if (mp == NULL) {
            error = ENOENT;
            break;
        }
        error = vfs_busy(mp, 0);
        vfs_rel(mp);
        if (error != 0)
            break;
        error = hfs_susp_suspend(mp, td);
        if (error != 0) {
            vfs_unbusy(mp);
            break;
        }
        priv = malloc(sizeof(struct cdevpriv*), M_TEMP, M_ZERO);
        *priv = (struct cdevpriv){ mp, td };
        error = devfs_set_cdevpriv(priv, hfs_susp_dtor);
        if (error != 0)
            hfs_susp_unsuspend(mp, td);
        break;
    case HFSIOC_RESUME:
        error = devfs_get_cdevpriv((void **)&priv);
        if (error != 0)
            break;
        /*
         * This calls hfs_susp_dtor, which in turn unsuspends the fs.
         * The dtor expects to be called without lock held, because
         * sometimes it's called from here, and sometimes due to the
         * file being closed or process exiting.
         */
        sx_xunlock(&hfs_susp_lock);
        devfs_clear_cdevpriv();
        return (0);
    default:
        error = ENXIO;
        break;
    }

    sx_xunlock(&hfs_susp_lock);

    return (error);
}

void
hfs_susp_initialize(void)
{
    sx_init(&hfs_susp_lock, "hfs_susp");
    hfs_susp_dev = make_dev(&hfs_susp_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600, "hfssuspend");
}

void
hfs_susp_uninitialize(void)
{
    destroy_dev(hfs_susp_dev);
    sx_destroy(&hfs_susp_lock);
}
