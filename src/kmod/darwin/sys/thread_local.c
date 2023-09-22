//
//  doc_tombstone_hash.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/20/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/eventhandler.h>

#define DOCHASH(td)    (&thlocal_hashtbl[(td)->td_tid & tbl_size_mask])

MALLOC_DEFINE(M_THLOCAL, "DocID Tombstone hash", "DocID Tombstone hash");

struct thread_local_entry {
    LIST_ENTRY(thread_local_entry) entry_link;
    struct thread *thread;
    void *data;
};

static LIST_HEAD(thread_local_head, thread_local_entry) *thlocal_hashtbl;
static u_long       tbl_size_mask;        /* size of hash table - 1 */
static struct mtx   tbl_mtx;
static eventhandler_tag thlocal_thread_dtor_tag;

static inline void tlock() {
    mtx_lock_spin(&tbl_mtx);
}

static inline void  tunlock(){
    mtx_unlock_spin(&tbl_mtx);
}

void*
thlocal_getdata(struct thread *td)
{
    struct thread_local_entry *entry;

    tlock();
    LIST_FOREACH(entry, DOCHASH(td), entry_link){
        if (entry->thread->td_tid == td->td_tid) {
            tunlock();
            return entry->data;
        }
    }
    tunlock();
    return (NULL);
}

void
thlocal_put(struct thread *td, void *data)
{
    struct thread_local_head *dth;
    struct thread_local_entry *entry;
    
    entry = malloc(sizeof(struct thread_local_entry), M_THLOCAL, M_ZERO);
    entry->data = data;
    entry->thread = td;
    
    tlock();
    dth = DOCHASH(td);
    LIST_INSERT_HEAD(dth, entry, entry_link);
    tunlock();
}

void
thlocal_remove(struct thread *td)
{
    struct thread_local_entry *entry;
    
    tlock();
    LIST_FOREACH(entry, DOCHASH(td), entry_link){
        if (entry->thread->td_tid == td->td_tid) {
            LIST_REMOVE(entry, entry_link);
            break;
        }
    }
    tunlock();
}

// clear the thread local data when the thread exits
static void
thlocal_thread_dtor(void *arg __unused, struct thread *td)
{
    struct thread_local_entry *entry;
    
    tlock();
    LIST_FOREACH(entry, DOCHASH(td), entry_link){
        if (entry->thread->td_tid == td->td_tid) {
            LIST_REMOVE(entry, entry_link);
            free(entry, M_THLOCAL);
        }
    }
    tunlock();
}

void thlocal_init(void)
{
    mtx_init(&tbl_mtx, "thlocal_mutex", "thlocal Lock", MTX_SPIN);
    thlocal_hashtbl = hashinit(PID_MAX, M_THLOCAL, &tbl_size_mask);
    thlocal_thread_dtor_tag = EVENTHANDLER_REGISTER(thread_dtor, thlocal_thread_dtor, NULL, EVENTHANDLER_PRI_ANY);
}

void thlocal_uninit(void)
{
    hashdestroy(thlocal_hashtbl, M_THLOCAL, tbl_size_mask);
    EVENTHANDLER_DEREGISTER(thread_dtor, thlocal_thread_dtor_tag);
    mtx_destroy(&tbl_mtx);
}
