# $FreeBSD$

.if !defined(SRCROOT)
SRCROOT		= ${.CURDIR}/../src
.endif

.PATH: ${SRCROOT}/${PROG}

PROG = hfs_util

# Enumerate Source files

SRCS    =	hfsutil_main.c hfsutil_jnl.c

MAN		=	hfs_util.8

# Remove -ansi since Darwin code uses C++ comments.
CFLAGS += -w

# Include program module makefile
.include <bsd.prog.mk>
