//
//  locks.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/16/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#include <kern/locks.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

static MALLOC_DEFINE(M_DARWIN_LOCKS, "darwin_locks", "Locks for darwin compat");

#pragma mark - lock attrs

lck_attr_t *
lck_attr_alloc_init(void)
{
    lck_attr_t * lckattr = malloc(sizeof(lck_attr_t), M_DARWIN_LOCKS, M_ZERO);
    return lckattr;
}

void
lck_attr_setdefault(lck_attr_t *attr)
{
}

void
lck_attr_setdebug(lck_attr_t *attr)
{
}

void
lck_attr_cleardebug(lck_attr_t *attr)
{
}

void
lck_attr_free(lck_attr_t *attr)
{
    free(attr, M_DARWIN_LOCKS);
}

#pragma mark - mutex

lck_mtx_t*
lck_mtx_alloc_init(lck_grp_t *grp, lck_attr_t *attr)
{
    lck_mtx_t * lck = malloc(sizeof(lck_mtx_t), M_DARWIN_LOCKS, M_ZERO);
    lck->grp = grp;
    lck->attr = attr;
    mtx_init(&lck->mtx, grp->grp_name, "lck_mtx_t Lock", MTX_DEF);
    return lck;
}

void
lck_mtx_init(lck_mtx_t *lck, lck_grp_t *grp, lck_attr_t *attr)
{
    KASSERT(lck, "null mutex passed");
    lck->grp = grp;
    lck->attr = attr;
    mtx_init(&lck->mtx, grp->grp_name, "lck_mtx_t Lock", MTX_DEF);
}

void
lck_mtx_lock(lck_mtx_t *lck)
{
    mtx_lock(&lck->mtx);
}

void
lck_mtx_unlock(lck_mtx_t *lck)
{
    mtx_unlock(&lck->mtx);
}

void
lck_mtx_destroy(lck_mtx_t *lck, lck_grp_t *grp)
{
    lck->attr = NULL;
    lck->grp = NULL;
    mtx_destroy(&lck->mtx);
}

void
lck_mtx_free(lck_mtx_t *lck, lck_grp_t *grp)
{
    free(lck, M_DARWIN_LOCKS);
}

#pragma mark - rw lock

#define LOCKMGR_RWINIT_FLAGS        (LC_SLEEPLOCK | LC_SLEEPABLE | LC_RECURSABLE | LC_UPGRADABLE)

lck_rw_t*
lck_rw_alloc_init(lck_grp_t *grp, lck_attr_t *attr)
{
    lck_rw_t * lck = malloc(sizeof(lck_rw_t), M_DARWIN_LOCKS, M_ZERO);
    lck->grp = grp;
    lck_rw_init(lck, grp, attr);
    return lck;
}

void
lck_rw_init(lck_rw_t *lck, lck_grp_t *grp, lck_attr_t *attr)
{
    KASSERT(lck, "null rwlock passed");
    lck->grp = grp;
    lck->attr = attr;
    lockinit(&lck->base, PVFS, grp->grp_name, VLKTIMEOUT, LOCKMGR_RWINIT_FLAGS);
}

void
lck_rw_lock(lck_rw_t *lck, lck_rw_type_t lck_rw_type)
{
    switch (lck_rw_type) {
        case LCK_RW_TYPE_SHARED:
            lockmgr(&lck->base, LCK_RW_TYPE_SHARED, NULL);
            break;
        case LCK_RW_TYPE_EXCLUSIVE:
            lockmgr(&lck->base, LCK_RW_TYPE_EXCLUSIVE, NULL);
            break;
        default:
            panic("Unknown lock type: %d", lck_rw_type);
    }
}

void
lck_rw_unlock(lck_rw_t *lck, lck_rw_type_t lck_rw_type)
{
    lockmgr(&lck->base, LK_RELEASE, NULL);
}

void
lck_rw_lock_shared(lck_rw_t *lck)
{
    lockmgr(&lck->base, LCK_RW_TYPE_SHARED, NULL);
}

void
lck_rw_unlock_shared(lck_rw_t *lck)
{
    lockmgr(&lck->base, LK_RELEASE, NULL);
}

boolean_t __attribute__((noreturn))
lck_rw_lock_yield_shared(lck_rw_t *lck, boolean_t force_yield)
{
    panic("unsupported function: %s", __func__);
}

void
lck_rw_lock_exclusive(lck_rw_t *lck)
{
    lockmgr(&lck->base, LCK_RW_TYPE_EXCLUSIVE, NULL);
}

void
lck_rw_unlock_exclusive(lck_rw_t *lck)
{
    lockmgr(&lck->base, LK_RELEASE, NULL);
}


boolean_t
lck_rw_lock_shared_to_exclusive(lck_rw_t *lck)
{
    return lockmgr(&lck->base, LK_UPGRADE, NULL);
}

void
lck_rw_lock_exclusive_to_shared(lck_rw_t *lck)
{
    lockmgr(&lck->base, LK_DOWNGRADE, NULL);
}

boolean_t
lck_rw_try_lock(lck_rw_t *lck, lck_rw_type_t lck_rw_type)
{
    switch (lck_rw_type) {
        case LCK_RW_TYPE_SHARED:
            return lockmgr(&lck->base, LCK_RW_TYPE_SHARED | LK_NOWAIT, NULL) == 0;
        case LCK_RW_TYPE_EXCLUSIVE:
            return lockmgr(&lck->base, LCK_RW_TYPE_EXCLUSIVE | LK_NOWAIT, NULL) == 0;
        default:
            panic("Unknown lock type: %d", lck_rw_type);
    }
}

boolean_t
lck_rw_try_lock_shared(lck_rw_t *lck)
{
    return lockmgr(&lck->base, LCK_RW_TYPE_SHARED | LK_NOWAIT, NULL) == 0;
}

boolean_t
lck_rw_try_lock_exclusive(lck_rw_t *lck)
{
    return lockmgr(&lck->base, LCK_RW_TYPE_EXCLUSIVE | LK_NOWAIT, NULL) == 0;
}


lck_rw_type_t
lck_rw_done(lck_rw_t *lck)
{
    panic("unsupported function: %s", __func__);
}

void
lck_rw_destroy(lck_rw_t *lck, lck_grp_t *grp)
{
    lck->attr = NULL;
    lck->grp = NULL;
    lockdestroy(&lck->base);
}

void
lck_rw_free(lck_rw_t *lck, lck_grp_t *grp)
{
    free(lck, M_DARWIN_LOCKS);
}


#pragma mark - spin lock


lck_spin_t *
lck_spin_alloc_init(lck_grp_t *grp, lck_attr_t *attr)
{
    lck_spin_t * spin = malloc(sizeof(lck_spin_t), M_DARWIN_LOCKS, M_ZERO);
    spin->grp = grp;
    spin->attr = attr;
    mtx_init(&spin->spin_mtx, grp->grp_name, "lck_spin_t Lock", MTX_SPIN);
    return spin;
}

void
lck_spin_init(lck_spin_t *lck, lck_grp_t *grp, lck_attr_t *attr)
{
    KASSERT(lck, "null spinlock passed");
    lck->grp = grp;
    lck->attr = attr;
    mtx_init(&lck->spin_mtx, grp->grp_name, "lck_spin_t Lock", MTX_SPIN);
}

void
lck_spin_lock(lck_spin_t *lck)
{
    mtx_lock_spin(&lck->spin_mtx);
}

void
lck_spin_lock_grp(lck_spin_t *lck,lck_grp_t *grp)
{
    mtx_lock_spin(&lck->spin_mtx);
}

void
lck_spin_unlock(lck_spin_t *lck)
{
    mtx_unlock_spin(&lck->spin_mtx);
}

void
lck_spin_destroy(lck_spin_t *lck, lck_grp_t *grp)
{
    lck->attr = NULL;
    lck->grp = NULL;
    mtx_destroy(&lck->spin_mtx);
}

void
lck_spin_free(lck_spin_t *lck, lck_grp_t *grp)
{
    free(lck, M_DARWIN_LOCKS);
}


