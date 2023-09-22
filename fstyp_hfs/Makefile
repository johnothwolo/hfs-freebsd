# $FreeBSD$

PROG = fstyp_hfs

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: ${SRCROOT}/${PROG}

# Enumerate Source files

SRCS    =	fstyp_hfs.c

MAN		=	fstyp_hfs.8

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include program makefile
.include <bsd.prog.mk>
