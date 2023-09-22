//
//  dkioctl.h
//  hfs-freebsd
//
//  Created by jothwolo on 8/29/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//

#ifndef dkioctl_h
#define dkioctl_h

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <err.h>
#include <sys/wait.h>
#include <sys/sysctl.h>

// include these before so they aren't included in the real source files.
// this ensures that the real prototypes of ioctl and fcntl are declared above the *_DARWIN macros below.
// if these are omitted then the files are included after the macro and the real protypes will be...
// renamed to fcntl_DARWIN/ioctl_DARWIN, causing a recursive call (worst case) or at best a linking error.

#include <sys/disk.h> // freebsd disallows includeing sys/ioctl.h directly.
#include <sys/fcntl.h>

// Hack the ioctl syscalls for wipefs.cpp (file is in libutil)

#define DKIOCGETBLOCKSIZE       0x10
#define DKIOCGETBLOCKCOUNT      0x12
#define DKIOCUNMAP              0x14
#define DKIOCSYNCHRONIZECACHE   0x18

#define F_GETPATH               0x20

typedef struct{
    uint64_t               offset;
    uint64_t               length;
} dk_extent_t;

typedef struct{
    dk_extent_t *          extents;
    uint32_t               extentsCount;

    uint32_t               options;

#ifndef __LP64__
    uint8_t                reserved0096[4];    /* reserved, clear to zero */
#endif /* !__LP64__ */
} dk_unmap_t;

static bool
candelete(int fd)
{
    struct diocgattr_arg arg;

    strlcpy(arg.name, "GEOM::candelete", sizeof(arg.name));
    arg.len = sizeof(arg.value.i);
    if (ioctl(fd, DIOCGATTR, &arg) == 0)
        return (arg.value.i != 0);
    else
        return (false);
}

static int
ioctl_DARWIN(int fd, unsigned long cmd, ...)
{
    int error = 0;
    uintptr_t data;
    va_list ap;
    
    va_start(ap, cmd);
    
    data = va_arg(ap, uint64_t);
    
    switch (cmd) {
        case DKIOCGETBLOCKSIZE:
            if ((error = ioctl(fd, DIOCGSECTORSIZE, data)) < 0) {
                warn("ioctl(DIOCGSECTORSIZE) failed");
            }
            break;
            
        case DKIOCGETBLOCKCOUNT:{
            off_t mediaSize;
            int sectorSize;
            if ((error = ioctl(fd, DIOCGMEDIASIZE, &mediaSize)) < 0) {
                warn("ioctl(DIOCGMEDIASIZE) failed");
            }
            
            if ((error = ioctl(fd, DIOCGSECTORSIZE, &sectorSize)) < 0) {
                warn("ioctl(DIOCGSECTORSIZE) failed");
            }
            
            if ((mediaSize % sectorSize) != 0) {
                warnx("sector size (%d) isn't multiple of media size (%lu)", sectorSize, mediaSize);
                error = EIO;
            }
            
            *(uint64_t*) data = mediaSize / sectorSize;
            break;
        }
        
        /* return values aren't tracked for the calls below */
        
        case DKIOCUNMAP:{
            dk_unmap_t *dk_unmap = (dk_unmap_t *)data;
            off_t arg[2];
            
            for (int i = 0; i < dk_unmap->extentsCount; i++) {
                // dk_unmap->options is currently unused in darwin
                arg[0] = dk_unmap->extents[i].offset;
                arg[1] = dk_unmap->extents[i].length;
                error = ioctl(fd, DIOCGDELETE, arg);
                if (error < 0) {
                    if (errno == EOPNOTSUPP && !candelete(fd)) {
                        warnx("TRIM/UNMAP not supported by driver");
                    } else
                        warn("ioctl(DIOCGDELETE) failed");
                    break;
                }
            }
            break;
        }
            
        case DKIOCSYNCHRONIZECACHE:
            error = ioctl(fd, DIOCGFLUSH);
            break;
        
        default:
            printf("impossible ioctl %lu", cmd);
            abort();
            __builtin_unreachable();
    }
    
    va_end(ap);
    
    if (error < 0) {
        printf("Error: %s", strerror(errno));
    }
    
    return error;
}


#define ioctl   ioctl_DARWIN

#endif /* dkioctl_h */
