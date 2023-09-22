/*
* Copyright (c) 2000, Todd C. Miller.  All rights reserved.
* Copyright (c) 1996, Jason Downs.  All rights reserved.
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
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT,
* INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*/

#include <fcntl.h>
#include "../util.h"
#include <unistd.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/uuid.h>
//#include <IOKit/IOBSD.h>
//#include <IOKit/IOKitLib.h>
//#include <IOKit/storage/IOMedia.h>

extern char debug;
extern void plog(const char *, ...);

/*
 * Given a uuid string, look up the BSD device and open it.
 * This code comes from DanM.
 *
 * Essentially, it is given a UUID string (from the journal header),
 * and then looks it up via IOKit.  From there, it then gets the
 * BSD name (e.g., /dev/dsik3), and opens it read-only.
 *
 * It returns the file descriptor, or -1 on error.
 */
int
OpenDeviceByUUID(void *uuidp, char **namep)
{
#if unsupported
    char devname[ PATH_MAX ];
    CFStringRef devname_string;
    int fd = -1;
    CFMutableDictionaryRef matching;
    io_service_t media;
    uuid_string_t uuid_cstring;
    CFStringRef uuid_string;

    memcpy(&uuid_cstring, uuidp, sizeof(uuid_cstring));

    uuid_string = CFStringCreateWithCString( kCFAllocatorDefault, uuid_cstring, kCFStringEncodingUTF8 );
    if ( uuid_string ) {
        matching = IOServiceMatching( kIOMediaClass );
        if ( matching ) {
            CFDictionarySetValue( matching, CFSTR( kIOMediaUUIDKey ), uuid_string );
            media = IOServiceGetMatchingService( kIOMasterPortDefault, matching );
            if ( media ) {
                devname_string = IORegistryEntryCreateCFProperty( media, CFSTR( kIOBSDNameKey ), kCFAllocatorDefault, 0 );
                if ( devname_string ) {
                    if ( CFStringGetCString( devname_string, devname, sizeof( devname ), kCFStringEncodingUTF8 ) ) {
			if (debug)
				plog("external journal device name = `%s'\n", devname);

                        fd = opendev( devname, O_RDONLY, 0, NULL );
			if (fd != -1 && namep != NULL) {
				*namep = strdup(devname);
			}
                    }
                    CFRelease( devname_string );
                }
                IOObjectRelease( media );
            }
            /* do not CFRelease( matching ); */
        }
        CFRelease( uuid_string );
    }
    return fd;
#endif
    return -1;
}
