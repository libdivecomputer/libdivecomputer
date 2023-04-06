SUBDIRS = include src

if ENABLE_EXAMPLES
SUBDIRS += examples
endif

if ENABLE_DOC
SUBDIRS += doc
endif

AM_MAKEFLAGS = -s
ACLOCAL_AMFLAGS = -I m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libdivecomputer.pc

EXTRA_DIST = \
	libdivecomputer.pc.in \
	contrib/README \
	contrib/android/Android.mk \
	contrib/msvc/libdivecomputer.vcxproj \
	contrib/msvc/libdivecomputer.vcxproj.filters \
	contrib/udev/libdivecomputer.rules
