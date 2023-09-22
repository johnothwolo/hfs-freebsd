# $FreeBSD$

PROG = fsck_hfs

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: 	${SRCROOT}/${PROG} \
		${SRCROOT}/${PROG}/dfalib

# Enumerate Source files

SRCS    =	cache.c						\
			fsck_debug.c				\
			fsck_hfs.c					\
			fsck_hfs_strings.c			\
			fsck_messages.c				\
			fsck_strings.c				\
			utilities.c					\

# dfalib

# DecompMakeData.c is a standalone tool

SRCS	+=	BTree.c					\
			BTreeAllocate.c			\
			BTreeMiscOps.c			\
			BTreeNodeOps.c			\
			BTreeScanner.c			\
			BTreeTreeOps.c			\
			BlockCache.c			\
			CatalogCheck.c			\
			HardLinkCheck.c			\
			SAllocate.c				\
			SBTree.c				\
			SCatalog.c				\
			SControl.c				\
			SDevice.c				\
			SExtents.c				\
			SKeyCompare.c			\
			SRebuildBTree.c			\
			SRepair.c				\
			SStubs.c				\
			SUtils.c				\
			SVerify1.c				\
			SVerify2.c				\
			VolumeBitmapCheck.c		\
			dirhardlink.c			\
			fsck_journal.c			\
			hfs_endian.c			\
			uuid.c

MAN		=	fsck_hfs.8

# Include directories
CFLAGS += -fblocks
CFLAGS += -I${SRCROOT}/kmod
CFLAGS += -I${SRCROOT}/kmod/darwin
CFLAGS += -I${SRCROOT}/libutil
CFLAGS += -DTRUE=true
CFLAGS += -DFALSE=false
CFLAGS += -DDEBUG_BUILD=1

LDFLAGS += -lBlocksRuntime -lpthread

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include program makefile
.include <bsd.prog.mk>
