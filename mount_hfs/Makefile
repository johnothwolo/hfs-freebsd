# $FreeBSD$

PROG = mount_hfs

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: ${SRCROOT}/${PROG}

# Enumerate Source files

SRCS    =   mount_hfs.c	\
			mntopts.c
			

MAN		=	mount_hfs.8

# Include directories
CFLAGS += -I${SRCROOT}/kmod
CFLAGS += -I${SRCROOT}/kmod/darwin
CFLAGS += -I${SRCROOT}/libutil

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include program makefile
.include <bsd.prog.mk>
