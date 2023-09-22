//
//  locks.h
//  hfs-freebsd
//
//  Created by jothwolo on 8/16/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#ifndef locks_h
#define locks_h

#include <sys/stdint.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/lockmgr.h>

#pragma mark - lock group attrs

typedef struct _lck_grp_attr_ {
    int dummy;
} lck_grp_attr_t;

typedef struct _lck_grp_ {
    lck_grp_attr_t *attr;
    char *grp_name;
} lck_grp_t;

__BEGIN_DECLS

lck_grp_attr_t* lck_grp_attr_alloc_init(void);
void lck_grp_attr_setdefault(lck_grp_attr_t *attr);
void lck_grp_attr_setstat(lck_grp_attr_t *attr);
void lck_grp_attr_free(lck_grp_attr_t *attr);

lck_grp_t* lck_grp_alloc_init(const char* grp_name, lck_grp_attr_t *attr);
void lck_grp_free(lck_grp_t *grp);

__END_DECLS

#pragma mark - lock attrs

typedef struct _lck_attr_ {
    int lkflags;
}lck_attr_t;

lck_attr_t *lck_attr_alloc_init(void);
void lck_attr_setdefault(lck_attr_t *attr);
void lck_attr_setdebug(lck_attr_t *attr);
void lck_attr_cleardebug(lck_attr_t *attr);
void lck_attr_free(lck_attr_t *attr);

#pragma mark - mutex

typedef struct _lck_mtx_ {
    lck_grp_t *grp;
    lck_attr_t *attr;
    struct mtx mtx;
} lck_mtx_t;

__BEGIN_DECLS

lck_mtx_t* lck_mtx_alloc_init(lck_grp_t *grp, lck_attr_t *attr);
void lck_mtx_init(lck_mtx_t *lck, lck_grp_t *grp, lck_attr_t *attr);
void lck_mtx_lock(lck_mtx_t *lck);
void lck_mtx_unlock(lck_mtx_t *lck);
void lck_mtx_destroy(lck_mtx_t *lck, lck_grp_t *grp);
void lck_mtx_free(lck_mtx_t *lck, lck_grp_t *grp);

__END_DECLS

#pragma mark - rw lock

typedef struct _lck_rw_ {
    lck_grp_t *grp;
    lck_attr_t *attr;
    struct lock base;
} lck_rw_t;

typedef enum {
    LCK_RW_TYPE_SHARED         = LK_SHARED,
    LCK_RW_TYPE_EXCLUSIVE      = LK_EXCLUSIVE,
} lck_rw_type_t;


__BEGIN_DECLS

lck_rw_t* lck_rw_alloc_init(lck_grp_t *grp, lck_attr_t *attr);
void lck_rw_init(lck_rw_t *lck, lck_grp_t *grp, lck_attr_t *attr);
void lck_rw_lock(lck_rw_t *lck, lck_rw_type_t lck_rw_type);
void lck_rw_unlock(lck_rw_t *lck, lck_rw_type_t lck_rw_type);
void lck_rw_lock_shared(lck_rw_t *lck);
void lck_rw_unlock_shared(lck_rw_t *lck);
boolean_t lck_rw_lock_yield_shared(lck_rw_t *lck, boolean_t force_yield);
void lck_rw_lock_exclusive(lck_rw_t *lck);
void lck_rw_unlock_exclusive(lck_rw_t *lck);

boolean_t lck_rw_lock_shared_to_exclusive(lck_rw_t *lck);
void lck_rw_lock_exclusive_to_shared(lck_rw_t *lck);
boolean_t lck_rw_try_lock(lck_rw_t *lck, lck_rw_type_t lck_rw_type);
boolean_t lck_rw_try_lock_shared(lck_rw_t *lck);
boolean_t lck_rw_try_lock_exclusive(lck_rw_t *lck);

lck_rw_type_t lck_rw_done(lck_rw_t *lck);
void lck_rw_destroy(lck_rw_t *lck, lck_grp_t *grp);
void lck_rw_free(lck_rw_t *lck, lck_grp_t *grp);

__END_DECLS

#pragma mark - spin lock

typedef struct _lck_spin_ {
    lck_grp_t *grp;
    lck_attr_t *attr;
    struct mtx spin_mtx;
} lck_spin_t;

__BEGIN_DECLS

lck_spin_t *lck_spin_alloc_init(lck_grp_t *grp, lck_attr_t *attr);
void  lck_spin_init(lck_spin_t *lck, lck_grp_t *grp, lck_attr_t *attr);
void  lck_spin_lock(lck_spin_t *lck);
void  lck_spin_lock_grp(lck_spin_t *lck,lck_grp_t *grp);
void  lck_spin_unlock(lck_spin_t *lck);
void  lck_spin_destroy(lck_spin_t *lck, lck_grp_t *grp);
void  lck_spin_free(lck_spin_t *lck, lck_grp_t *grp);

__END_DECLS

#endif /* locks_h */
