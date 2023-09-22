# $FreeBSD$

PROG = newfs_hfs

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: ${SRCROOT}/${PROG}

# link with openssl
LIBADD	=	ssl crypto c++

# Enumerate Source files

SRCS    =   hfs_endian.c	\
			makehfs.c		\
			newfs_hfs.c		\

SRCS	+=	wipefs.cpp			\
			ExtentManager.cpp

MAN		=	newfs_hfs.8

# Include directories
CFLAGS += -I${SRCROOT}/kmod
CFLAGS += -I${SRCROOT}/kmod/darwin
CFLAGS += -I${SRCROOT}/libutil

LDFLAGS += -lc++ -lssl -lcrypto

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include program makefile
.include <bsd.prog.mk>
