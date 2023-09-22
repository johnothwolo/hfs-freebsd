# $FreeBSD$

PROG = fsck_hfs

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: ${SRCROOT}/${PROG}

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

SRCS	+=	dfalib/BTree.c				\
			dfalib/BTreeAllocate.c		\
			dfalib/BTreeMiscOps.c		\
			dfalib/BTreeNodeOps.c		\
			dfalib/BTreeScanner.c		\
			dfalib/BTreeTreeOps.c		\
			dfalib/BlockCache.c			\
			dfalib/CatalogCheck.c		\
			dfalib/HardLinkCheck.c		\
			dfalib/SAllocate.c			\
			dfalib/SBTree.c				\
			dfalib/SCatalog.c			\
			dfalib/SControl.c			\
			dfalib/SDevice.c			\
			dfalib/SExtents.c			\
			dfalib/SKeyCompare.c		\
			dfalib/SRebuildBTree.c		\
			dfalib/SRepair.c			\
			dfalib/SStubs.c				\
			dfalib/SUtils.c				\
			dfalib/SVerify1.c			\
			dfalib/SVerify2.c			\
			dfalib/VolumeBitmapCheck.c	\
			dfalib/dirhardlink.c		\
			dfalib/fsck_journal.c		\
			dfalib/hfs_endian.c			\
			dfalib/uuid.c

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
