# Note: It is important to make sure you include the <bsd.kmod.mk> makefile
# after declaring the KMOD and SRCS variables.

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: 	${SRCROOT}/kmod 				\
		${SRCROOT}/kmod/darwin			\
		${SRCROOT}/kmod/darwin/vfs		\
		${SRCROOT}/kmod/darwin/hfs		\
		${SRCROOT}/kmod/darwin/kern		\
		${SRCROOT}/kmod/darwin/uuid		\
		${SRCROOT}/kmod/darwin/sys		\
		${SRCROOT}/kmod/darwin/os		\
		${SRCROOT}/kmod/hfs_encodings	\
		${SRCROOT}/kmod/core			\
		${SRCROOT}/kmod/geom/label		\


# define macro KERNEL=_KERNEL (define both...)
# define TARGET_OS_OSX macro
# add include for "SRCROOT/darwin/"
# define macro MODULE_NAME=hfs

# Exclude:
#   hfs_attrlist.c
#   hfs_cprotect.c

# Declare Name of kernel module
KMOD	=	hfs

#### Enumerate Source files for kernel module ####

# required header include
SRCS	= 	vnode_if.h							

# compatibility files
# darwin/sys/
SRCS   +=   compat.c					\
			doc_tombstone.c			\
			fsevents.c				\
			utfconv.c				\
			thread_local.c			\
			vfs_name.c				\
# darwin/uuid/
SRCS   +=   uuid.c					
# darwin/kern
SRCS   +=   locks.c					\
			lck_grp.c				

# HFS 

# ./hfs_encodings/*
# I think separate modules have to be created to support multiple encodings.
SRCS   +=	hfs_encodings.c		\
			hfs_encodinghint.c	

# ./core/*
# this replace hfs_iokit.cpp
SRCS   +=	hfs_geom.c	
SRCS   +=	BTree.c							\
			BTreeAllocate.c					\
			BTreeNodeOps.c					\
			BTreeMiscOps.c					\
			BTreeNodeReserve.c				\
			BTreeScanner.c					\
			BTreeWrapper.c					\
			BTreeTreeOps.c					\
			CatalogUtilities.c				\
			FileExtentMapping.c				\
			FileIDsServices.c				\
			MacOSStubs.c					\
			UnicodeWrappers.c				\
			VolumeAllocation.c				\
			hfs_btreeio.c					\
			hfs_journal.c					\
			hfs_lookup.c					\
			hfs_catalog.c					\
			hfs_suspend.c					\
			hfs_vnops.c						\
			hfs_vfsops.c					\
			hfs_endian.c					\
			hfs_link.c						\
			hfs_xattr.c						\
			hfs_hotfiles.c					\
			hfs_notification.c				\
			hfs_readwrite.c					\
			hfs_resize.c					\
			hfs_chash.c						\
			hfs_vfsutils.c					\
			hfs_cnode.c						\
			rangelist.c

# ./geom/label
# geom label support
SRCS   +=	g_label_hfs.c

# main kmod file
SRCS   +=	hfs_kmod.c		
			

# TODO: maybe separate geom module?
# TODO: What about encoding module?
# TODO: and darwin compat module? idk

# Debug flags 	= DEBUG=1 $(inherited) _KERNEL __LP64__ KERNEL TARGET_OS_OSX
# Release flags = _KERNEL __LP64__ KERNEL TARGET_OS_OSX
DEBUG_FLAGS += -g -O0 -ggdb
CFLAGS  = -D DEBUG=1
CFLAGS += -D _KERNEL=1
CFLAGS += -D KERNEL=1
CFLAGS += -D __LP64__=1
CFLAGS += -D TARGET_OS_OSX=1

# Include directories
CFLAGS += -I${SRCROOT}/kmod/darwin
CFLAGS += -I${SRCROOT}/kmod

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include kernel module makefile
.include <bsd.kmod.mk>
