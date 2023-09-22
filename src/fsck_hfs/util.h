//
//  util.h
//  hfs-freebsd
//
//  Created by jothwolo on 8/29/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#ifndef util_h
#define util_h

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <malloc.h>

enum {
    OPENDEV_PART   = 0x01,        /* Try to open the raw partition. */
    OPENDEV_BLCK   = 0x04,        /* Open block, not character device. */
};

#ifndef    _PATH_DEV
#define    _PATH_DEV    "/dev/"
#endif

/*
 * opendev(3) is an inherently non-thread-safe API, since
 * it returns a buffer to global storage. However we can
 * at least make sure the storage allocation is thread safe
 * and does not leak memory in case of simultaneous
 * initialization
 */
static pthread_once_t opendev_namebuf_once = PTHREAD_ONCE_INIT;
static char *namebuf = NULL;

static void opendev_namebuf_init(void);

static int
opendev(
    char *path,
    int oflags,
    int dflags,
    char **realpath
){
    int fd;
    char *slash, *prefix;

    /* Initial state */
    if (realpath)
        *realpath = path;
    fd = -1;
    errno = ENOENT;

    if (pthread_once(&opendev_namebuf_once,
                     opendev_namebuf_init)
        || !namebuf) {
        errno = ENOMEM;
        return -1;
    }

    if (dflags & OPENDEV_BLCK)
        prefix = "";            /* block device */
    else
        prefix = "r";            /* character device */

    if ((slash = strchr(path, '/')))
        fd = open(path, oflags);
    else if (dflags & OPENDEV_PART) {
        if (snprintf(namebuf, PATH_MAX, "%s%s%s",
            _PATH_DEV, prefix, path) < PATH_MAX) {
            char *slice;
            while ((slice = strrchr(namebuf, 's')) &&
                isdigit(*(slice-1))) *slice = '\0';
            fd = open(namebuf, oflags);
            if (realpath)
                *realpath = namebuf;
        } else
            errno = ENAMETOOLONG;
    }
    if (!slash && fd == -1 && errno == ENOENT) {
        if (snprintf(namebuf, PATH_MAX, "%s%s%s",
            _PATH_DEV, prefix, path) < PATH_MAX) {
            fd = open(namebuf, oflags);
            if (realpath)
                *realpath = namebuf;
        } else
            errno = ENAMETOOLONG;
    }
    return (fd);
}

static void opendev_namebuf_init(void)
{
    namebuf = malloc(PATH_MAX);
}


#endif /* util_h */
