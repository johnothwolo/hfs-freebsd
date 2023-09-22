/*-
* SPDX-License-Identifier: BSD-2-Clause-FreeBSD
*
* Copyright © 2023-present jothwolo.
* All rights reserved.
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
* THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
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
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <kern/locks.h>

static MALLOC_DEFINE(M_DARWIN_LOCK_GROUP, "darwin_locks", "Locks for darwin compat");

static lck_grp_attr_t lck_grp_attr_dummy = {0};

lck_grp_attr_t *
lck_grp_attr_alloc_init(void)
{
    return &lck_grp_attr_dummy;
}

void
lck_grp_attr_setdefault(lck_grp_attr_t *attr)
{
}

void
lck_grp_attr_setstat(lck_grp_attr_t *attr)
{
}

void
lck_grp_attr_free(lck_grp_attr_t *attr)
{
}

lck_grp_t *
lck_grp_alloc_init(const char* grp_name, lck_grp_attr_t *attr)
{
    lck_grp_t * grp = malloc(sizeof(lck_grp_attr_t), M_DARWIN_LOCK_GROUP, M_ZERO);
    grp->grp_name = strdup(grp_name, M_DARWIN_LOCK_GROUP);
    return grp;
}

void
lck_grp_free(lck_grp_t *grp)
{
    KASSERT(grp, "lck_grp_t is null");
    free(grp->grp_name, M_DARWIN_LOCK_GROUP);
    free(grp, M_DARWIN_LOCK_GROUP);
}
