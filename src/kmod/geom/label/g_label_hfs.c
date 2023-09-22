//
//  g_label_hfs.c
//  hfs-freebsd
//
//  Created by jothwolo on 8/28/23.
//  Copyright © 2023-present jothwolo. All rights reserved.
//  This file is covered under the MPL2.0. See LICENSE file for more details.
//


#include <sys/param.h>
#include <sys/bio.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include <geom/geom.h>
#include <geom/geom_dbg.h>
#include <geom/label/g_label.h>

#define HFS_PRI_SECTOR(blksize)          (1024 / (blksize))
#define HFS_PRI_OFFSET(blksize)          ((blksize) > 1024 ? 1024 : 0)

/* Signatures used to differentiate between HFS and HFS Plus volumes */
enum {
    kHFSSigWord        = 0x4244,    /* 'BD' in ASCII */
    kHFSPlusSigWord        = 0x482B,    /* 'H+' in ASCII */
    kHFSJSigWord        = 0x484a,    /* 'HJ' in ASCII */
    kHFSPlusVersion        = 0x0004,    /* will change as format changes */
                        /* version 4 shipped with Mac OS 8.1 */
    kHFSPlusMountVersion    = 0x31302E30,    /* '10.0' for Mac OS X */
    kHFSJMountVersion    = 0x4846534a    /* 'HFSJ' for journaled HFS+ on OS X */
};

enum {
    kHFSMaxVolumeNameChars        = 27,
    kHFSMaxFileNameChars        = 31,
    kHFSPlusMaxFileNameChars    = 255
};

/* HFS extent descriptor */
typedef struct HFSExtentDescriptor {
    u_int16_t     startBlock;        /* first allocation block */
    u_int16_t     blockCount;        /* number of allocation blocks */
} HFSExtentDescriptor;

/* HFS extent record */
typedef HFSExtentDescriptor HFSExtentRecord[3];

/* HFS Master Directory Block - 162 bytes */
/* Stored at sector #2 (3rd sector) and second-to-last sector. */
typedef struct HFSMasterDirectoryBlock {
    u_int16_t         drSigWord;    /* == kHFSSigWord */
    u_int32_t         drCrDate;    /* date and time of volume creation */
    u_int32_t         drLsMod;    /* date and time of last modification */
    u_int16_t         drAtrb;        /* volume attributes */
    u_int16_t         drNmFls;    /* number of files in root folder */
    u_int16_t         drVBMSt;    /* first block of volume bitmap */
    u_int16_t         drAllocPtr;    /* start of next allocation search */
    u_int16_t         drNmAlBlks;    /* number of allocation blocks in volume */
    u_int32_t         drAlBlkSiz;    /* size (in bytes) of allocation blocks */
    u_int32_t         drClpSiz;    /* default clump size */
    u_int16_t         drAlBlSt;    /* first allocation block in volume */
    u_int32_t         drNxtCNID;    /* next unused catalog node ID */
    u_int16_t         drFreeBks;    /* number of unused allocation blocks */
    u_char             drVN[kHFSMaxVolumeNameChars + 1];  /* volume name */
    u_int32_t         drVolBkUp;    /* date and time of last backup */
    u_int16_t         drVSeqNum;    /* volume backup sequence number */
    u_int32_t         drWrCnt;    /* volume write count */
    u_int32_t         drXTClpSiz;    /* clump size for extents overflow file */
    u_int32_t         drCTClpSiz;    /* clump size for catalog file */
    u_int16_t         drNmRtDirs;    /* number of directories in root folder */
    u_int32_t         drFilCnt;    /* number of files in volume */
    u_int32_t         drDirCnt;    /* number of directories in volume */
    u_int32_t         drFndrInfo[8];    /* information used by the Finder */
    u_int16_t         drEmbedSigWord;    /* embedded volume signature (formerly drVCSize) */
    HFSExtentDescriptor    drEmbedExtent;    /* embedded volume location and size (formerly drVBMCSize and drCtlCSize) */
    u_int32_t        drXTFlSize;    /* size of extents overflow file */
    HFSExtentRecord        drXTExtRec;    /* extent record for extents overflow file */
    u_int32_t         drCTFlSize;    /* size of catalog file */
    HFSExtentRecord     drCTExtRec;    /* extent record for catalog file */
} HFSMasterDirectoryBlock;

/* Function to detect and read HFS volume name */
static void
g_label_hfs_taste(struct g_consumer *cp, char *label, size_t size)
{
    struct g_provider *pp;
    HFSMasterDirectoryBlock *fs;
    char *s_volume_name;

    g_topology_assert_not();
    pp = cp->provider;
    label[0] = '\0';

    KASSERT(pp->sectorsize != 0, ("Tasting a disk with 0 sectorsize"));\

    fs = g_read_data(cp, HFS_PRI_SECTOR(pp->sectorsize), pp->sectorsize, NULL);
    if (fs == NULL)
        return;
        
    bool isver = 0; // be16toh(fs->version) != kHFSPlusVersion;
    
    // FIXME: Also get VolumeHeader
    if (((be16toh(fs->drEmbedSigWord) != kHFSPlusSigWord &&
        be16toh(fs->drEmbedSigWord) != kHFSJSigWord) || isver) &&
        be16toh(fs->drSigWord) == kHFSSigWord) {
        
        // FIXME: get from VolumeHeader
        printf("hfs: mount: sig 0x%x and version 0x%x are not HFS or HFS+.\n", fs->drSigWord, (uint16_t)fs->drCrDate);
        goto exit_free;
    } else {
        // TODO: be more specific
        G_LABEL_DEBUG(1, "HFS/HFS+ file system detected on %s.", pp->name);
    }
    
    s_volume_name = (char*)fs->drVN;
    /* Terminate label */
    s_volume_name[sizeof(fs->drVN) - 1] = '\0';

    if (s_volume_name[0] == '/')
        s_volume_name += 1;

    /* Check for volume label */
    if (s_volume_name[0] == '\0')
        goto exit_free;

    strlcpy(label, s_volume_name, size);

exit_free:
    g_free(fs);
}

/* Define HFS label class */
struct g_label_desc g_label_hfs = {
    .ld_taste = g_label_hfs_taste,
    .ld_dirprefix = "hfs/",
    .ld_enabled = 1
};

G_LABEL_INIT(hfs, g_label_hfs, "Create device nodes for HFS volumes");
