AM_CPPFLAGS = -I$(top_builddir)/include -I$(top_srcdir)/include
LDADD = $(top_builddir)/src/libdivecomputer.la

bin_PROGRAMS = \
	universal \
	ostc-fwupdate

COMMON = common.c common.h \
	utils.c utils.h

universal_SOURCES = universal.c $(COMMON)

ostc_fwupdate_SOURCES = hw_ostc_fwupdate.c $(COMMON)
