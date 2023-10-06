/*
 * Copyright (c) 2000-2017 Apple Inc. All rights reserved.
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>


#include "hfs.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_endian.h"
#include "hfs_btreeio.h"

#include "FileMgrInternal.h"
#include "BTreesPrivate.h"

#if unsupported
/* From bsd/vfs/vfs_bio.c */
extern int bdwrite_internal(struct buf *, int);
#endif

static int ClearBTNodes(struct vnode *vp, int blksize, off_t offset, off_t amount);
static int btree_journal_modify_block_end(struct hfsmount *hfsmp, struct buf *bp);

void btree_swap_node(struct buf *bp);

/* 
 * Return btree node size for given vnode.
 *
 * Returns: 
 * 	For btree vnode, returns btree node size. 
 * 	For non-btree vnodes, returns 0.
 */
u_int16_t get_btree_nodesize(struct vnode *vp)
{
	BTreeControlBlockPtr btree;
	u_int16_t node_size = 0; 

	if ((vp)->v_iflag & VV_SYSTEM) {
		btree = (BTreeControlBlockPtr) VTOF(vp)->fcbBTCBPtr;
		if (btree) {
			node_size = btree->nodeSize;
		}
	}

	return node_size;
}

int SetBTreeBlockSize(FileReference vp, ByteCount blockSize, __unused ItemCount minBlockCount)
{
	BTreeControlBlockPtr	bTreePtr;
	
	hfs_assert(vp != NULL);
	hfs_assert(blockSize >= kMinNodeSize);
    if (blockSize > MAXBSIZE )
        return (fsBTBadNodeSize);

	bTreePtr = (BTreeControlBlockPtr)VTOF(vp)->fcbBTCBPtr;
	bTreePtr->nodeSize = blockSize;
	
    return (E_NONE);
}


int GetBTreeBlock(FileReference vp, u_int32_t blockNum, GetBlockOptions options, BlockDescriptor *block)
{
    int	 retval = E_NONE;
    struct buf   *bp = NULL;
	u_int8_t     allow_empty_node;	  

	/* If the btree block is being read using hint, it is 
	 * fine for the swap code to find zeroed out nodes. 
	 */
	if (options & kGetBlockHint) {
			allow_empty_node = true;
	} else {
			allow_empty_node = false;
	}

    if (options & kGetEmptyBlock) {
        daddr_t blkno;
        off_t offset;

        offset = (daddr_t)blockNum * (daddr_t)block->blockSize;
        bp = getblk(vp, (daddr_t)blockNum, block->blockSize, 0, 0, 0);
        
		if (bp && !VOP_BMAP(vp, blockNum, NULL, &blkno, NULL, NULL)) {
            bp->b_blkno = blkno;
        }
    } else {
        retval = bread(vp, (daddr_t)blockNum, block->blockSize, NOCRED, &bp);
    }
    if (bp == NULL)
        retval = -1;	//XXX need better error

    if (retval == E_NONE) {
        block->blockHeader = bp;
        block->buffer = (char *)(bp->b_data);
    	block->blockNum = bp->b_lblkno;
        block->blockReadFromDisk = ((bp->b_flags & B_CACHE) == 0);	/* not found in cache ==> came from disk */

		// XXXdbg 
		block->isModified = 0;

		/* Check and endian swap B-Tree node (only if it's a valid block) */
		if (!(options & kGetEmptyBlock)) {

			/* This happens when we first open the b-tree, we might not have all the node data on hand */
			if ((((BTNodeDescriptor *)block->buffer)->kind == kBTHeaderNode) &&
					(((BTHeaderRec *)((char *)block->buffer + 14))->nodeSize != (bp->b_bcount)) &&
					(SWAP_BE16 (((BTHeaderRec *)((char *)block->buffer + 14))->nodeSize) != (bp->b_bcount))) {

				/*
				 * Don't swap the node descriptor, record offsets, or other records.
				 * This record will be invalidated and re-read with the correct node
				 * size once the B-tree control block is set up with the node size
				 * from the header record.
				 */
				retval = hfs_swap_BTNode (block, vp, kSwapBTNodeHeaderRecordOnly, allow_empty_node);

			} else {
				/*
				 * In this case, we have enough data in-hand to do basic validation
				 * on the B-Tree node.
				 */
				if (block->blockReadFromDisk) {
					/*
					 * The node was just read from disk, so always swap/check it.
					 * This is necessary on big endian since the test below won't trigger.
					 */
					retval = hfs_swap_BTNode (block, vp, kSwapBTNodeBigToHost, allow_empty_node);
				} 
				else {
					/*
					 * Block wasn't read from disk; it was found in the cache.  
					 */
					if (*((u_int16_t *)((char *)block->buffer + (block->blockSize - sizeof (u_int16_t)))) == 0x0e00) {
						/*
						 * The node was left in the cache in non-native order, so swap it.
						 * This only happens on little endian, after the node is written
						 * back to disk.
						 */
						retval = hfs_swap_BTNode (block, vp, kSwapBTNodeBigToHost, allow_empty_node);
					}
					else if (*((u_int16_t *)((char *)block->buffer + (block->blockSize - sizeof (u_int16_t)))) == 0x000e) {
						/*
						 * The node was in-cache in native-endianness.  We don't need to do 
						 * anything here, because the node is ready to use.  Set retval == 0.
						 */
						retval = 0;
					}
					/*
					 * If the node doesn't have hex 14 (0xe) in the last two bytes of the buffer, 
					 * it doesn't necessarily mean that this is a bad node.  Zeroed nodes that are
					 * marked as unused in the b-tree map node would be OK and not have valid content.
					 */
				}
			}

			/*
			 * If we got an error, then the node is only partially swapped.
			 * We mark the buffer invalid so that the next attempt to get the
			 * node will read it and attempt to swap again, and will notice
			 * the error again.  If we didn't do this, the next attempt to get
			 * the node might use the partially swapped node as-is.
			 */
			if (retval)
				(bp)->b_flags |= B_INVAL;
		}
	}
    
    if (retval) {
    	if (bp)
			brelse(bp);
        block->blockHeader = NULL;
        block->buffer = NULL;
    }

    return (retval);
}


void ModifyBlockStart(FileReference vp, BlockDescPtr blockPtr)
{
	struct hfsmount	*hfsmp = VTOHFS(vp);
    struct buf *bp = NULL;

	if (hfsmp->jnl == NULL) {
		return;
	}
	
    bp = (struct buf *) blockPtr->blockHeader;
    if (bp == NULL) {
		panic("hfs: ModifyBlockStart: null bp  for blockdescptr %p?!?\n", blockPtr);
		return;
    }

	journal_modify_block_start(hfsmp->jnl, bp);
	blockPtr->isModified = 1;
}

void
btree_swap_node(struct buf *bp)
{
    //	struct hfsmount *hfsmp = (struct hfsmount *)bp->b_fsprivate2;
	int retval;
    struct vnode *vp = bp->b_vp;
    BlockDescriptor block;
				    
    /* Prepare the block pointer */
    block.blockHeader = bp;
    block.buffer = (char *)bp->b_data;
    block.blockNum = bp->b_lblkno;
    /* not found in cache ==> came from disk */
    block.blockReadFromDisk = ((bp->b_flags & B_CACHE) == 0);
    block.blockSize = (uint32_t) bp->b_bcount;

    /* Swap the data now that this node is ready to go to disk.
     * We allow swapping of zeroed out nodes here because we might
     * be writing node whose last record just got deleted.
     */
    retval = hfs_swap_BTNode (&block, vp, kSwapBTNodeHostToBig, true);
    if (retval)
    	panic("hfs: btree_swap_node: about to write corrupt node!\n");
}


static int
btree_journal_modify_block_end(struct hfsmount *hfsmp, struct buf *bp)
{
    return journal_modify_block_end(hfsmp->jnl, bp, btree_swap_node, hfsmp);
}


int ReleaseBTreeBlock(FileReference vp, BlockDescPtr blockPtr, ReleaseBlockOptions options)
{
    struct hfsmount	*hfsmp = VTOHFS(vp);
    int	retval = E_NONE;
    struct buf *bp = NULL;

    bp = (struct buf *) blockPtr->blockHeader;

    if (bp == NULL) {
        retval = -1;
        goto exit;
    }

    if (options & kTrashBlock) {
                (bp)->b_flags |= B_INVAL;

		if (hfsmp->jnl && (BUF_ISLOCKED(bp))) {
			journal_kill_block(hfsmp->jnl, bp);
		} else {
			brelse(bp);	/* note: B-tree code will clear blockPtr->blockHeader and blockPtr->buffer */
		}
		
		/* Don't let anyone else try to use this bp, it's been consumed */
		blockPtr->blockHeader = NULL;
		
    } else {
        if (options & kForceWriteBlock) {
			if (hfsmp->jnl) {
				if (blockPtr->isModified == 0) {
					panic("hfs: releaseblock: modified is 0 but forcewrite set! bp %p\n", bp);
				}

				retval = btree_journal_modify_block_end(hfsmp, bp);
				blockPtr->isModified = 0;
			} else {
				retval = bwrite(bp);
			}
			
			/* Don't let anyone else try to use this bp, it's been consumed */
			blockPtr->blockHeader = NULL;
			
        } else if (options & kMarkBlockDirty) {
			struct timeval tv;
			microuptime(&tv);
            if ((options & kLockTransaction) && hfsmp->jnl == NULL) {
                /*
                 *
                 * Set the B_LOCKED flag and unlock the buffer, causing brelse to move
                 * the buffer onto the LOCKED free list.  This is necessary, otherwise
                 * getnewbuf() would try to reclaim the buffers using buf_bawrite, which
                 * isn't going to work.
                 *
                 */
                /* Don't hog all the buffers... */

                if (buf_dirty_count_severe() > kMaxLockedMetaBuffers) {
                     hfs_btsync(vp, HFS_SYNCTRANS);
                     /* Rollback sync time to cause a sync on lock release... */
                     (void) BTSetLastSync(VTOF(vp), (uint32_t) tv.tv_sec - (kMaxSecsForFsync + 1));
                }

                BUF_LOCK(bp, LK_EXCLUSIVE | LK_NOWAIT | LK_CANRECURSE, NULL);
            }

            /* 
             * Delay-write this block.
             * If the maximum delayed buffers has been exceeded then
             * free up some buffers and fall back to an asynchronous write.
             */
			if (hfsmp->jnl) {
				if (blockPtr->isModified == 0) {
					panic("hfs: releaseblock: modified is 0 but markdirty set! bp %p\n", bp);
				}
				retval = btree_journal_modify_block_end(hfsmp, bp);
				blockPtr->isModified = 0;
			}
#if unsupported
            else if (bdwrite_internal(bp, 1) != 0) {
                hfs_btsync(vp, 0);
                /* Rollback sync time to cause a sync on lock release... */
                (void) BTSetLastSync(VTOF(vp), (uint32_t) tv.tv_sec - (kMaxSecsForFsync + 1));

                BUF_UNLOCK(bp);
                bawrite(bp);
            }
#endif
            bdwrite(bp);
            
            /* Don't let anyone else try to use this bp, it's been consumed */
			blockPtr->blockHeader = NULL;
			
        } else {
			// check if we had previously called journal_modify_block_start() 
			// on this block and if so, abort it (which will call brelse()).
			if (hfsmp->jnl && blockPtr->isModified) {
				// XXXdbg - I don't want to call modify_block_abort()
				//          because I think it may be screwing up the
				//          journal and blowing away a block that has
				//          valid data in it.
				//   
				//    journal_modify_block_abort(hfsmp->jnl, bp);
				//panic("hfs: releaseblock called for 0x%x but mod_block_start previously called.\n", bp);
				btree_journal_modify_block_end(hfsmp, bp);
				blockPtr->isModified = 0;
			} else {
				brelse(bp);	/* note: B-tree code will clear blockPtr->blockHeader and blockPtr->buffer */
			}
			
			/* Don't let anyone else try to use this bp, it's been consumed */
			blockPtr->blockHeader = NULL;
        }
    }

exit:
    return (retval);
}


int ExtendBTreeFile(FileReference vp, FSSize minEOF, FSSize maxEOF)
{
#pragma unused (maxEOF)

	int	retval = 0, ret = 0;
	int64_t		actualBytesAdded, origSize;
	u_int64_t	bytesToAdd;
	u_int32_t	startAllocation;
	u_int32_t	fileblocks;
	BTreeInfoRec 	btInfo;
	ExtendedVCB	*vcb;
	FCB		*filePtr;
    struct thread 	*td = curthread;
	int64_t 	trim = 0;
	int  		lockflags = 0;

	filePtr = GetFileControlBlock(vp);

	if ( (off_t)minEOF > filePtr->fcbEOF )
	{
		bytesToAdd = minEOF - filePtr->fcbEOF;

		if (bytesToAdd < filePtr->ff_clumpsize)
			bytesToAdd = filePtr->ff_clumpsize;		//XXX why not always be a mutiple of clump size?
	}
	else
	{
		return -1;
	}

	vcb = VTOVCB(vp);
	
	/*
	 * The Extents B-tree can't have overflow extents. ExtendFileC will
	 * return an error if an attempt is made to extend the Extents B-tree
	 * when the resident extents are exhausted.
	 */

	/* Protect allocation bitmap and extents overflow file. */
	lockflags = SFL_BITMAP;
	if (VTOC(vp)->c_fileid != kHFSExtentsFileID)
		lockflags |= SFL_EXTENTS;
	lockflags = hfs_systemfile_lock(vcb, lockflags, HFS_EXCLUSIVE_LOCK);

	(void) BTGetInformation(filePtr, 0, &btInfo);

#if 0  // XXXdbg
	/*
	 * The b-tree code expects nodes to be contiguous. So when
	 * the allocation block size is less than the b-tree node
	 * size, we need to force disk allocations to be contiguous.
	 */
	if (vcb->blockSize >= btInfo.nodeSize) {
		extendFlags = 0;
	} else {
		/* Ensure that all b-tree nodes are contiguous on disk */
		extendFlags = kEFContigMask;
	}
#endif

	origSize = filePtr->fcbEOF;
	fileblocks = filePtr->ff_blocks;
	startAllocation = vcb->nextAllocation;

	// loop trying to get a contiguous chunk that's an integer multiple
	// of the btree node size.  if we can't get a contiguous chunk that
	// is at least the node size then we break out of the loop and let
	// the error propagate back up.
	while((off_t)bytesToAdd >= btInfo.nodeSize) {
	    do {
		retval = ExtendFileC(vcb, filePtr, bytesToAdd, 0,
		                     kEFContigMask | kEFMetadataMask | kEFNoClumpMask,
		                     (int64_t *)&actualBytesAdded);
		if (retval == dskFulErr && actualBytesAdded == 0) {
		    bytesToAdd >>= 1;
		    if (bytesToAdd < btInfo.nodeSize) {
			break;
		    } else if ((bytesToAdd % btInfo.nodeSize) != 0) {
			// make sure it's an integer multiple of the nodeSize
			bytesToAdd -= (bytesToAdd % btInfo.nodeSize);
		    }
		}
	    } while (retval == dskFulErr && actualBytesAdded == 0);

	    if (retval == dskFulErr && actualBytesAdded == 0 && bytesToAdd <= btInfo.nodeSize) {
		break;
	    }

	    filePtr->fcbEOF = (u_int64_t)filePtr->ff_blocks * (u_int64_t)vcb->blockSize;
	    bytesToAdd = minEOF - filePtr->fcbEOF;
	}

	/*
	 * If a new extent was added then move the roving allocator
	 * reference forward by the current b-tree file size so 
	 * there's plenty of room to grow.
	 */
	if ((retval == 0) &&
	    ((VCBTOHFS(vcb)->hfs_flags & HFS_METADATA_ZONE) == 0) &&
	    (vcb->nextAllocation > startAllocation) &&
	    ((vcb->nextAllocation + fileblocks) < vcb->allocLimit)) {
		HFS_UPDATE_NEXT_ALLOCATION(vcb, vcb->nextAllocation + fileblocks); 
	}
		
	filePtr->fcbEOF = (u_int64_t)filePtr->ff_blocks * (u_int64_t)vcb->blockSize;

	// XXXdbg ExtendFileC() could have returned an error even though
	// it grew the file to be big enough for our needs.  If this is
	// the case, we don't care about retval so we blow it away.
	//
	if (filePtr->fcbEOF >= (off_t)minEOF && retval != 0) {
		retval = 0;
	}

	// XXXdbg if the file grew but isn't large enough or isn't an
	// even multiple of the nodeSize then trim things back.  if
	// the file isn't large enough we trim back to the original
	// size.  otherwise we trim back to be an even multiple of the
	// btree node size.
	//
	if ((filePtr->fcbEOF < (off_t)minEOF) || ((filePtr->fcbEOF - origSize) % btInfo.nodeSize) != 0) {

		if (filePtr->fcbEOF < (off_t)minEOF) {
			retval = dskFulErr;
			
			if (filePtr->fcbEOF < origSize) {
                panic("hfs: btree file eof %ld less than orig size %ld!\n",
					  filePtr->fcbEOF, origSize);
			}
			
			trim = filePtr->fcbEOF - origSize;
		} else {
			trim = ((filePtr->fcbEOF - origSize) % btInfo.nodeSize);
		}

		ret = TruncateFileC(vcb, filePtr, filePtr->fcbEOF - trim, 0, 0, FTOC(filePtr)->c_fileid, 0);
		filePtr->fcbEOF = (u_int64_t)filePtr->ff_blocks * (u_int64_t)vcb->blockSize;

		// XXXdbg - panic if the file didn't get trimmed back properly
		if ((filePtr->fcbEOF % btInfo.nodeSize) != 0) {
            panic("hfs: truncate file didn't! fcbEOF %ld nsize %d fcb %p\n",
				  filePtr->fcbEOF, btInfo.nodeSize, filePtr);
		}

		if (ret) {
			// XXXdbg - this probably doesn't need to be a panic()
            panic("hfs: error truncating btree files (sz 0x%lx, trim %ld, ret %ld)\n",
			      filePtr->fcbEOF, trim, (long)ret);
//			goto out;
		}
	}

	if(VTOC(vp)->c_fileid != kHFSExtentsFileID) {
		/*
		 * Get any extents overflow b-tree changes to disk ASAP!
		 */
		(void) BTFlushPath(VTOF(vcb->extentsRefNum));
		(void) hfs_fsync(vcb->extentsRefNum, MNT_WAIT, 0, td);
	}
	hfs_systemfile_unlock(vcb, lockflags);
	lockflags = 0;

	if ((filePtr->fcbEOF % btInfo.nodeSize) != 0) {
        panic("hfs: extendbtree: fcb %p has eof 0x%lx not a multiple of 0x%x (trim %lx)\n",
			  filePtr, filePtr->fcbEOF, btInfo.nodeSize, trim);
	}

	/*
	 * Update the Alternate MDB or Alternate VolumeHeader
	 */
	VTOC(vp)->c_flag |= C_MODIFIED;
	if ((VTOC(vp)->c_fileid == kHFSExtentsFileID)	||
	    (VTOC(vp)->c_fileid == kHFSCatalogFileID)	||
	    (VTOC(vp)->c_fileid == kHFSAttributesFileID)
	   ) {
		MarkVCBDirty( vcb );
		ret = hfs_flushvolumeheader(VCBTOHFS(vcb), HFS_FVH_WAIT | HFS_FVH_WRITE_ALT);
	} else {
		VTOC(vp)->c_touch_chgtime = TRUE;
		VTOC(vp)->c_touch_modtime = TRUE;
		(void) hfs_update(vp, 0);
	}

	ret = ClearBTNodes(vp, btInfo.nodeSize, origSize, (filePtr->fcbEOF - origSize));
//out:
	if (retval == 0)
		retval = ret;
	
	if (lockflags)
		hfs_systemfile_unlock(vcb, lockflags);
	
	return retval;
}


/*
 * Clear out (zero) new b-tree nodes on disk.
 */
static int
ClearBTNodes(struct vnode *vp, int blksize, off_t offset, off_t amount)
{
	struct hfsmount *hfsmp = VTOHFS(vp);
	struct buf *bp = NULL;
	daddr_t blk;
	daddr_t blkcnt;
    
	blk = offset / blksize;
	blkcnt = amount / blksize;
	
	while (blkcnt > 0) {
		bp = getblk(vp, blk, blksize, 0, 0, 0);
		if (bp == NULL)
			continue;

        // XXXdbg
		if (hfsmp->jnl) {
			// XXXdbg -- skipping this for now since it makes a transaction
			//           become *way* too large
		    //journal_modify_block_start(hfsmp->jnl, bp);
		}
		bzero((char *)bp->b_data, blksize);

        // XXXdbg
		if (hfsmp->jnl) {
			// XXXdbg -- skipping this for now since it makes a transaction
			//           become *way* too large
			//journal_modify_block_end(hfsmp->jnl, bp);

			// XXXdbg - remove this once we decide what to do with the
			//          writes to the journal
			if ((blk % 32) == 0)
			    bwrite(bp);
			else
			    bawrite(bp);
		} else {
			/* wait/yield every 32 blocks so we don't hog all the buffers */
			if ((blk % 32) == 0)
				bwrite(bp);
			else
				bawrite(bp);
		}
        bqrelse(bp);
		--blkcnt;
		++blk;
	}

	return (0);
}


extern char  hfs_attrname[];

/*
 * Create an HFS+ Attribute B-tree File.
 *
 * No global resources should be held.
 */
int
hfs_create_attr_btree(struct hfsmount *hfsmp, u_int32_t nodesize, u_int32_t nodecnt)
{
	struct vnode* vp = NULLVP;
	struct cat_desc cndesc;
	struct cat_attr cnattr;
	struct cat_fork cfork;
	BlockDescriptor blkdesc;
	BTNodeDescriptor  *ndp;
	BTHeaderRec  *bthp;
	BTreeControlBlockPtr btcb = NULL;
	struct buf *bp = NULL;
	void * buffer;
	u_int8_t *bitmap;
	u_int16_t *index;
	u_int32_t node_num, num_map_nodes;
	u_int32_t bytes_per_map_record;
	u_int32_t temp;
	u_int16_t  offset;
	int intrans = 0;
	int result;
	int newvnode_flags = 0;
	
again:
	/*
	 * Serialize creation using HFS_CREATING_BTREE flag.
	 */
	hfs_lock_mount (hfsmp);
	if (hfsmp->hfs_flags & HFS_CREATING_BTREE) {
			/* Someone else beat us, wait for them to finish. */
        (void) msleep(&hfsmp->hfs_attribute_cp, &hfsmp->hfs_mutex,
			              PDROP | PINOD, "hfs_create_attr_btree", 0);
			if (hfsmp->hfs_attribute_vp) {
				return (0);
			}
			goto again;
	}
	hfsmp->hfs_flags |= HFS_CREATING_BTREE;
	hfs_unlock_mount (hfsmp);

	/* Check if were out of usable disk space. */
	if ((hfs_freeblks(hfsmp, 1) == 0)) {
		result = ENOSPC;
		goto exit;
	}

	/*
	 * Set up Attribute B-tree vnode
	 * (this must be done before we start a transaction
	 *  or take any system file locks)
	 */
	bzero(&cndesc, sizeof(cndesc));
	cndesc.cd_parentcnid = kHFSRootParentID;
	cndesc.cd_flags |= CD_ISMETA;
	cndesc.cd_nameptr = (const u_int8_t *)hfs_attrname;
	cndesc.cd_namelen = strlen(hfs_attrname);
	cndesc.cd_cnid = kHFSAttributesFileID;

	bzero(&cnattr, sizeof(cnattr));
	cnattr.ca_linkcount = 1;
	cnattr.ca_mode = S_IFREG;
	cnattr.ca_fileid = cndesc.cd_cnid;

	bzero(&cfork, sizeof(cfork));
	cfork.cf_clump = nodesize * nodecnt;

	result = hfs_getnewvnode(hfsmp, NULL, NULL, &cndesc, gnv_dfl, &cnattr, &cfork, &vp, &newvnode_flags);
	if (result) {
		goto exit;
	}
	/*
	 * Set up Attribute B-tree control block
	 */
	btcb = hfs_mallocz(sizeof(*btcb));

	btcb->nodeSize          = nodesize;
	btcb->maxKeyLength      = kHFSPlusAttrKeyMaximumLength;
	btcb->btreeType         = 0xFF;
	btcb->attributes        = kBTVariableIndexKeysMask | kBTBigKeysMask;
	btcb->version           = kBTreeVersion;
	btcb->writeCount        = 1;
	btcb->flags             = 0;  /* kBTHeaderDirty */
	btcb->fileRefNum        = vp;
	btcb->getBlockProc      = GetBTreeBlock;
	btcb->releaseBlockProc  = ReleaseBTreeBlock;
	btcb->setEndOfForkProc  = ExtendBTreeFile;
	btcb->keyCompareProc    = (KeyCompareProcPtr)hfs_attrkeycompare;
	
	/* 
	 * NOTE: We must make sure to zero out this pointer if we error out in this function!
	 * If we don't, then unmount will treat it as a valid pointer which can lead to a
	 * use-after-free 
	 */
	VTOF(vp)->fcbBTCBPtr    = btcb;

	/*
	 * Allocate some space
	 */
	if (hfs_start_transaction(hfsmp) != 0) {
		result = EINVAL;
		goto exit;
	}
	intrans = 1;

	/* Note ExtendBTreeFile will acquire the necessary system file locks. */
	result = ExtendBTreeFile(vp, nodesize, cfork.cf_clump);
	if (result)
		goto exit;

	btcb->totalNodes = (uint32_t) VTOF(vp)->ff_size / nodesize;

	/*
	 * Figure out how many map nodes we'll need.
	 *
	 * bytes_per_map_record = the number of bytes in the map record of a
	 * map node.  Since that is the only record in the node, it is the size
	 * of the node minus the node descriptor at the start, and two record
	 * offsets at the end of the node.  The "- 2" is to round the size down
	 * to a multiple of 4 bytes (since sizeof(BTNodeDescriptor) is not a
	 * multiple of 4).
	 *
	 * The value "temp" here is the number of *bits* in the map record of
	 * the header node.
	 */
	bytes_per_map_record = nodesize - sizeof(BTNodeDescriptor) - 2*sizeof(u_int16_t) - 2;
	temp = 8 * (nodesize - sizeof(BTNodeDescriptor) 
			- sizeof(BTHeaderRec)
			- kBTreeHeaderUserBytes
			- 4 * sizeof(u_int16_t));
	if (btcb->totalNodes > temp) {
		num_map_nodes = howmany(btcb->totalNodes - temp, bytes_per_map_record * 8);
	}
	else {
		num_map_nodes = 0;
	}
	
	btcb->freeNodes = btcb->totalNodes - 1 - num_map_nodes;
	
	/*
	 * Initialize the b-tree header on disk
	 */
	bp = getblk(vp, 0, nodesize, 0, 0, 0);
	if (bp == NULL) {
		result = EIO;
		goto exit;
	}

	buffer = (void *)bp->b_data;
	blkdesc.buffer = buffer;
	blkdesc.blockHeader = (void *)bp;
	blkdesc.blockReadFromDisk = 0;
	blkdesc.isModified = 0;

	ModifyBlockStart(vp, &blkdesc);

	if (bp->b_bufsize != nodesize)
		panic("hfs_create_attr_btree: bad buffer size (%ld)\n", bp->b_bufsize);

	bzero(buffer, nodesize);
	index = (u_int16_t *)buffer;

	/* FILL IN THE NODE DESCRIPTOR:  */
	ndp = (BTNodeDescriptor *)buffer;
	if (num_map_nodes != 0)
		ndp->fLink = 1;
	ndp->kind = kBTHeaderNode;
	ndp->numRecords = 3;
	offset = sizeof(BTNodeDescriptor);
	index[(nodesize / 2) - 1] = offset;

	/* FILL IN THE HEADER RECORD:  */
	bthp = (BTHeaderRec *)((u_int8_t *)buffer + offset);
	bthp->nodeSize     = nodesize;
	bthp->totalNodes   = btcb->totalNodes;
	bthp->freeNodes    = btcb->freeNodes;
	bthp->clumpSize    = cfork.cf_clump;
	bthp->btreeType    = 0xFF;
	bthp->attributes   = kBTVariableIndexKeysMask | kBTBigKeysMask;
	bthp->maxKeyLength = kHFSPlusAttrKeyMaximumLength;
	bthp->keyCompareType = kHFSBinaryCompare;
	offset += sizeof(BTHeaderRec);
	index[(nodesize / 2) - 2] = offset;

	/* FILL IN THE USER RECORD:  */
	offset += kBTreeHeaderUserBytes;
	index[(nodesize / 2) - 3] = offset;

	/* Mark the header node and map nodes in use in the map record.
	 *
	 * NOTE: Assumes that the header node's map record has at least
	 * (num_map_nodes + 1) bits.
	 */
	bitmap = (u_int8_t *) buffer + offset;
	temp = num_map_nodes + 1;	/* +1 for the header node */
	while (temp >= 8) {
		*(bitmap++) = 0xFF;
		temp -= 8;
	}
	*bitmap = ~(0xFF >> temp);
	
	offset += nodesize - sizeof(BTNodeDescriptor) - sizeof(BTHeaderRec)
			   - kBTreeHeaderUserBytes - (4 * sizeof(int16_t));
	index[(nodesize / 2) - 4] = offset;

	if (hfsmp->jnl) {
		result = btree_journal_modify_block_end(hfsmp, bp);
	} else {
		result = bwrite(bp);
	}
	if (result)
		goto exit;

	/* Create the map nodes: node numbers 1 .. num_map_nodes */
	for (node_num=1; node_num <= num_map_nodes; ++node_num) {
		bp = getblk(vp, node_num, nodesize, 0, 0, 0);
		if (bp == NULL) {
			result = EIO;
			goto exit;
		}
		buffer = (void *)bp->b_data;
		blkdesc.buffer = buffer;
		blkdesc.blockHeader = (void *)bp;
		blkdesc.blockReadFromDisk = 0;
		blkdesc.isModified = 0;
	
		ModifyBlockStart(vp, &blkdesc);
		
		bzero(buffer, nodesize);
		index = (u_int16_t *)buffer;
	
		/* Fill in the node descriptor */
		ndp = (BTNodeDescriptor *)buffer;
		if (node_num != num_map_nodes)
			ndp->fLink = node_num + 1;
		ndp->kind = kBTMapNode;
		ndp->numRecords = 1;
		offset = sizeof(BTNodeDescriptor);
		index[(nodesize / 2) - 1] = offset;
	
	
		/* Fill in the map record's offset */
		/* Note: We assume that the map record is all zeroes */
		offset = sizeof(BTNodeDescriptor) + bytes_per_map_record;
		index[(nodesize / 2) - 2] = offset;
	
		if (hfsmp->jnl) {
			result = btree_journal_modify_block_end(hfsmp, bp);
		} else {
			result = bwrite(bp);
		}
		if (result)
			goto exit;
	}
	
	/* Update vp/cp for attribute btree */
	hfs_lock_mount (hfsmp);
	hfsmp->hfs_attribute_cp = VTOC(vp);
	hfsmp->hfs_attribute_vp = vp;
	hfs_unlock_mount (hfsmp);

	(void) hfs_flushvolumeheader(hfsmp, HFS_FVH_WAIT | HFS_FVH_WRITE_ALT);

	if (intrans) {
		hfs_end_transaction(hfsmp);
		intrans = 0;
	}

	/* Initialize the vnode for virtual attribute data file */
	result = init_attrdata_vnode(hfsmp);
	if (result) {
		printf("hfs_create_attr_btree: vol=%s init_attrdata_vnode() error=%d\n", hfsmp->vcbVN, result); 
	}

exit:

	if (vp && result) {
		/* 
		 * If we're about to error out, then make sure to zero out the B-Tree control block pointer
		 * from the filefork of the EA B-Tree cnode/vnode. Failing to do this will lead to a use
		 * after free at unmount or BTFlushPath. Since we're about to error out anyway, this memory
		 * will be freed.
		 */
		VTOF(vp)->fcbBTCBPtr = NULL;
	}
	

	if (vp) {
		hfs_unlock(VTOC(vp));
	}
	if (result) {
		hfs_free(btcb, sizeof(*btcb));
		if (vp) {
            vrele(vp);
		}
		/* XXX need to give back blocks ? */
	}
	if (intrans) {
		hfs_end_transaction(hfsmp);
	}

	/*
	 * All done, clear HFS_CREATING_BTREE, and wake up any sleepers.
	 */
	hfs_lock_mount (hfsmp);
	hfsmp->hfs_flags &= ~HFS_CREATING_BTREE;
	wakeup((caddr_t)&hfsmp->hfs_attribute_cp);
	hfs_unlock_mount (hfsmp);

	return (result);
}

