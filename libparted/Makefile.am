# This file is part of GNU Parted
# Copyright (C) 1999-2001, 2007, 2009-2010 Free Software Foundation,
# Inc.
#
# This file may be modified and/or distributed without restriction.

SUBDIRS_CHECK =
if HAVE_CHECK
SUBDIRS_CHECK += tests
endif

ARCH_SOURCE = arch/$(OS).c
if BUILD_LINUX
ARCH_SOURCE += arch/$(OS).h
endif

AM_CFLAGS = $(WARN_CFLAGS) $(WERROR_CFLAGS)

SUBDIRS       = labels fs . $(SUBDIRS_CHECK)


partedincludedir      = -I$(top_srcdir)/lib -I$(top_srcdir)/include
lib_LTLIBRARIES	      =	libparted.la

# Set the shared library version, per Libtool's guidelines.
# For details, see the "Libtool's versioning system" section of "info libtool"
CURRENT = 0
REVISION = 1
AGE = 0

libparted_la_LDFLAGS  = -version-info $(CURRENT):$(REVISION):$(AGE)

libparted_la_SOURCES  = debug.c			\
			architecture.c		\
			architecture.h		\
			device.c		\
			exception.c		\
			filesys.c		\
			libparted.c		\
			timer.c			\
			unit.c			\
			disk.c			\
			cs/geom.c		\
			cs/constraint.c		\
			cs/natmath.c		\
			$(ARCH_SOURCE)


EXTRA_libparted_la_SOURCES    = arch/linux.c	\
				arch/linux.h	\
				arch/gnu.c	\
				arch/beos.c

libparted_la_LIBADD =	\
  fs/libfs.la		\
  labels/liblabels.la	\
  $(top_builddir)/lib/libgnulib.la \
  $(OS_LIBS)		\
  $(DL_LIBS)		\
  $(DM_LIBS)		\
  $(SELINUX_LIBS)	\
  $(LIB_BLKID)		\
  $(INTLLIBS)

EXTRA_DIST	      = mbr.s

INCLUDES = $(partedincludedir) $(INTLINCS)

MAINTAINERCLEANFILES = Makefile.in
