/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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
#ifndef _SYS_DECMPFS_H_
#define _SYS_DECMPFS_H_ 1

#include <sys/types.h>
#include <kern/locks.h>
#include <sys/vnode.h>

// FIXME: remove
struct vnop_pagein_args {};
struct vnop_read_args {};
struct vop_bwrite_args{};

/*
 * Please switch on @DECMPFS_ENABLE_KDEBUG_TRACES to enable tracepoints.
 * Tracepoints are compiled out by default to eliminate any overhead due to
 * kernel tracing.
 *
 * #define DECMPFS_ENABLE_KDEBUG_TRACES 1
 */
#if DECMPFS_ENABLE_KDEBUG_TRACES
#define DECMPFS_EMIT_TRACE_ENTRY(D, ...) \
    KDBG_FILTERED((D) | DBG_FUNC_START, ## __VA_ARGS__)
#define DECMPFS_EMIT_TRACE_RETURN(D, ...) \
    KDBG_FILTERED((D) | DBG_FUNC_END, ##__VA_ARGS__)
#else
#define DECMPFS_EMIT_TRACE_ENTRY(D, ...) do {} while (0)
#define DECMPFS_EMIT_TRACE_RETURN(D, ...) do {} while (0)
#endif /* DECMPFS_ENABLE_KDEBUG_TRACES */

/*
 * KERNEL_DEBUG related definitions for decmpfs.
 *
 * Please NOTE: The Class DBG_FSYSTEM = 3, and Subclass DBG_DECMP = 0x12, so
 * these debug codes are of the form 0x0312nnnn.
 */
#define DECMPDBG_CODE(code)  FSDBG_CODE(DBG_DECMP, code)

enum {
    DECMPDBG_DECOMPRESS_FILE            = 0x03120000, // DECMPDBG_CODE(0),/* 0x03120000 */
    DECMPDBG_FETCH_COMPRESSED_HEADER    = 0x03120004, // DECMPDBG_CODE(1),/* 0x03120004 */
    DECMPDBG_FETCH_UNCOMPRESSED_DATA    = 0x03120008, // DECMPDBG_CODE(2),/* 0x03120008 */
    DECMPDBG_FREE_COMPRESSED_DATA       = 0x03120010, // DECMPDBG_CODE(4),/* 0x03120010 */
    DECMPDBG_FILE_IS_COMPRESSED         = 0x03120014, // DECMPDBG_CODE(5),/* 0x03120014 */
};

#define MAX_DECMPFS_XATTR_SIZE 3802

/*
 *  NOTE:  decmpfs can only be used by thread-safe filesystems
 */

#define DECMPFS_MAGIC 0x636d7066 /* cmpf */

#define DECMPFS_XATTR_NAME "com.apple.decmpfs" /* extended attribute to use for decmpfs */

typedef struct __attribute__((packed)) {
    /* this structure represents the xattr on disk; the fields below are little-endian */
    uint32_t compression_magic;
    uint32_t compression_type; /* see the enum below */
    uint64_t uncompressed_size;
    unsigned char attr_bytes[0]; /* the bytes of the attribute after the header */
} decmpfs_disk_header;

typedef struct __attribute__((packed)) {
    /* this structure represents the xattr in memory; the fields below are host-endian */
    uint32_t attr_size;
    uint32_t compression_magic;
    uint32_t compression_type;
    uint64_t uncompressed_size;
    unsigned char attr_bytes[0]; /* the bytes of the attribute after the header */
} decmpfs_header;

/* compression_type values */
enum {
    CMP_Type1       = 1,/* uncompressed data in xattr */

    /* additional types defined in AppleFSCompression project */

    CMP_MAX         = 255/* Highest compression_type supported */
};

typedef struct {
    void *buf;
    ssize_t size;
} decmpfs_vector;

#ifdef KERNEL

struct decmpfs_cnode {
    uint8_t cmp_state;
    uint8_t cmp_minimal_xattr;   /* if non-zero, this file's com.apple.decmpfs xattr contained only the minimal decmpfs_disk_header */
    uint32_t cmp_type;
    uint32_t lockcount;
    void    *lockowner;          /* cnode's lock owner (if a thread is currently holding an exclusive lock) */
    uint64_t uncompressed_size __attribute__((aligned(8)));
    uint64_t decompression_flags;
    lck_rw_t compressed_data_lock;
};



typedef struct decmpfs_cnode decmpfs_cnode;

/* return values from decmpfs_file_is_compressed */
enum {
    FILE_TYPE_UNKNOWN      = 0,
    FILE_IS_NOT_COMPRESSED = 1,
    FILE_IS_COMPRESSED     = 2,
    FILE_IS_CONVERTING     = 3/* file is converting from compressed to decompressed */
};

/* vfs entrypoints */
extern struct thread* decmpfs_ctx;

/* client filesystem entrypoints */
void decmpfs_init(void);
void decmpfs_uninit(void);
decmpfs_cnode *decmpfs_cnode_alloc(void);
void decmpfs_cnode_free(decmpfs_cnode *dp);
void decmpfs_cnode_init(decmpfs_cnode *cp);
void decmpfs_cnode_destroy(decmpfs_cnode *cp);

int decmpfs_hides_rsrc(struct thread * ctx, decmpfs_cnode *cp);
int decmpfs_hides_xattr(struct thread * ctx, decmpfs_cnode *cp, const char *xattr);

bool decmpfs_trylock_compressed_data(decmpfs_cnode *cp, int exclusive);
void decmpfs_lock_compressed_data(decmpfs_cnode *cp, int exclusive);
void decmpfs_unlock_compressed_data(decmpfs_cnode *cp, int exclusive);

uint32_t decmpfs_cnode_get_vnode_state(decmpfs_cnode *cp);
void decmpfs_cnode_set_vnode_state(decmpfs_cnode *cp, uint32_t state, int skiplock);
uint64_t decmpfs_cnode_get_vnode_cached_size(decmpfs_cnode *cp);
uint32_t decmpfs_cnode_cmp_type(decmpfs_cnode *cp);

int decmpfs_file_is_compressed(struct vnode * vp, decmpfs_cnode *cp);
int decmpfs_validate_compressed_file(struct vnode * vp, decmpfs_cnode *cp);
int decmpfs_decompress_file(struct vnode * vp, decmpfs_cnode *cp, off_t toSize, int truncate_okay, int skiplock);  /* if toSize == -1, decompress the entire file */
int decmpfs_free_compressed_data(struct vnode * vp, decmpfs_cnode *cp);
int decmpfs_update_attributes(struct vnode * vp, struct vattr *vap);
/* the following two routines will set *is_compressed to 0 if the file was converted from compressed to decompressed before data could be fetched from the decompressor */
int decmpfs_pagein_compressed(struct vnop_pagein_args *ap, int *is_compressed, decmpfs_cnode *cp);
int decmpfs_read_compressed(struct vnop_read_args *ap, int *is_compressed, decmpfs_cnode *cp);

/* types shared between the kernel and kexts */
typedef int (*decmpfs_validate_compressed_file_func)(struct vnode * vp, struct thread * ctx, decmpfs_header *hdr);
typedef void (*decmpfs_adjust_fetch_region_func)(struct vnode * vp, struct thread * ctx, decmpfs_header *hdr, off_t *offset, ssize_t *size);
typedef int (*decmpfs_fetch_uncompressed_data_func)(struct vnode * vp, struct thread * ctx, decmpfs_header *hdr, off_t offset, ssize_t size, int nvec, decmpfs_vector *vec, uint64_t *bytes_read);
typedef int (*decmpfs_free_compressed_data_func)(struct vnode * vp, struct thread * ctx, decmpfs_header *hdr);
typedef uint64_t (*decmpfs_get_decompression_flags_func)(struct vnode * vp, struct thread * ctx, decmpfs_header *hdr); // returns flags from the DECMPFS_FLAGS enumeration belowd

enum {
    DECMPFS_FLAGS_FORCE_FLUSH_ON_DECOMPRESS = 1 << 0,
};

/* Versions that are supported for binary compatibility */
#define DECMPFS_REGISTRATION_VERSION_V1 1
#define DECMPFS_REGISTRATION_VERSION_V3 3

#define DECMPFS_REGISTRATION_VERSION (DECMPFS_REGISTRATION_VERSION_V3)

typedef struct {
    int                                   decmpfs_registration;
    decmpfs_validate_compressed_file_func validate;
    decmpfs_adjust_fetch_region_func      adjust_fetch;
    decmpfs_fetch_uncompressed_data_func  fetch;
    decmpfs_free_compressed_data_func     free_data;
    decmpfs_get_decompression_flags_func  get_flags;
} decmpfs_registration;

/* hooks for kexts to call */
int register_decmpfs_decompressor(uint32_t compression_type, const decmpfs_registration *registration);
int unregister_decmpfs_decompressor(uint32_t compression_type, decmpfs_registration *registration);

#endif /* KERNEL */

#endif /* _SYS_DECMPFS_H_ */
