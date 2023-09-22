//
//  vfs_name.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/22/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//
//

#include "compat.h"

#include <sys/namei.h>
#include <sys/disk.h>
#include <sys/ucred.h>
#include <sys/buf.h>
#include <sys/bio.h>
#include <sys/malloc.h>
#include <sys/fnv_hash.h>

#define NAMETABLEHASH(nchash)    (&name_hashtbl[nchash & tbl_mask])

MALLOC_DEFINE(M_NAME_HASH, "XNU-style namecache", "XNU-style namecache");

struct name_entry {
    LIST_ENTRY(name_entry) entry_link;
    const char *name;
    uint64_t nchash;
    uint32_t refcount;
};

static hashtable_max = 10240; // TODO: make it a tunable?
static LIST_HEAD(thread_local_head, name_entry) *name_hashtbl;
static u_long       tbl_mask;        /* size of hash table - 1 */
static struct mtx   tbl_mtx;

static inline void tlock() {
    mtx_lock_spin(&tbl_mtx);
}

static inline void  tunlock(){
    mtx_unlock_spin(&tbl_mtx);
}

const char *
vfs_addname(const char *name, uint32_t len, uint32_t __unused nc_hash, uint32_t flags)
{
    boolean_t found = false;
    struct name_entry *entry = NULL;
    uint64_t hash = fnv_32_buf(name, len, FNV1_32_INIT);
    const char *ret = NULL;
    
    tlock();
    LIST_FOREACH(entry, NAMETABLEHASH(hash), entry_link){
        if (strncmp(entry->name, name, len)) {
            found = true;
        }
    }
    
    if (found){
        atomic_add_int((volatile u_int *)&entry->refcount, 1);
        ret = entry->name;
    } else {
        // insert entry
        entry = malloc(sizeof(struct name_entry), M_NAME_HASH, M_ZERO);
        entry->name = name;
        entry->nchash = hash;
        LIST_INSERT_HEAD(NAMETABLEHASH(hash), entry, entry_link);
    }
    
    tunlock();
    return ret;
}

int
vfs_removename(const char *name)
{
    boolean_t found = false;
    struct name_entry *entry = NULL;
    uint64_t hash = fnv_32_str(name, FNV1_32_INIT);
    int ret = -1;
    
    tlock();
    LIST_FOREACH(entry, NAMETABLEHASH(hash), entry_link){
        if (strcmp(entry->name, name)) {
            found = true;
        }
    }
    
    if (found){
        atomic_add_int((volatile u_int *)&entry->refcount, -1);
        ret = 0;
        
        if (entry->refcount == 0){
            free(entry, M_NAME_HASH);
        }
    }
    
    tunlock();
    return ret;
}

void
vfs_names_init(void)
{
    struct name_entry *entry, *tmp;
    int i;

    name_hashtbl = hashinit(hashtable_max, M_NAME_HASH, &tbl_mask);
    mtx_init(&tbl_mtx, "name_cache_mutex", "tbl_mtx Lock", MTX_SPIN);
    
    tlock();
    for (i = 0; i < tbl_mask; i++) {
        LIST_FOREACH_SAFE(entry, &name_hashtbl[i], entry_link, tmp) {
            LIST_REMOVE(entry, entry_link);
            free(entry, M_NAME_HASH);
        }
    }
    tunlock();
}

void
vfs_names_destroy(void)
{
    struct name_entry *entry, *tmp;
    int i;
    
    tlock();
    for (i = 0; i < tbl_mask; i++) {
        LIST_FOREACH_SAFE(entry, &name_hashtbl[i], entry_link, tmp) {
            LIST_REMOVE(entry, entry_link);
            free(entry, M_NAME_HASH);
        }
    }
    tunlock();
    hashdestroy(name_hashtbl, M_NAME_HASH, hashtable_max);
}
