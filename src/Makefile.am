#
# Public header files.
#

libdivecomputerdir = $(includedir)/libdivecomputer
libdivecomputer_HEADERS = 	\
	version.h \
	utils.h \
	buffer.h \
	device.h \
	parser.h \
	datetime.h \
	units.h \
	suunto.h \
	suunto_solution.h \
	suunto_eon.h \
	suunto_vyper.h \
	suunto_vyper2.h  \
	suunto_d9.h \
	reefnet.h \
	reefnet_sensus.h \
	reefnet_sensuspro.h \
	reefnet_sensusultra.h \
	uwatec.h \
	uwatec_aladin.h \
	uwatec_memomouse.h \
	uwatec_smart.h \
	oceanic.h \
	oceanic_atom2.h \
	oceanic_veo250.h \
	oceanic_vtpro.h \
	mares.h \
	mares_nemo.h \
	mares_puck.h \
	mares_darwin.h \
	mares_iconhd.h \
	hw.h \
	hw_ostc.h \
	cressi.h \
	cressi_edy.h \
	zeagle.h \
	zeagle_n2ition3.h \
	atomics.h \
	atomics_cobalt.h

#
# Source files.
#

AM_CFLAGS = $(LIBUSB_CFLAGS)

lib_LTLIBRARIES = libdivecomputer.la

libdivecomputer_la_LIBADD = $(LIBUSB_LIBS)
libdivecomputer_la_LDFLAGS = \
	-version-info $(DC_VERSION_LIBTOOL) \
	-no-undefined \
	-export-symbols libdivecomputer.exp

libdivecomputer_la_SOURCES = \
	version.c \
	device.h device-private.h device.c \
	parser.h parser-private.h parser.c \
	datetime.h datetime.c \
	suunto.h \
	suunto_common.h suunto_common.c \
	suunto_common2.h suunto_common2.c \
	suunto_solution.h suunto_solution.c suunto_solution_parser.c \
	suunto_eon.h suunto_eon.c suunto_eon_parser.c \
	suunto_vyper.h suunto_vyper.c suunto_vyper_parser.c \
	suunto_vyper2.h suunto_vyper2.c \
	suunto_d9.h suunto_d9.c suunto_d9_parser.c \
	reefnet.h \
	reefnet_sensus.h reefnet_sensus.c reefnet_sensus_parser.c \
	reefnet_sensuspro.h reefnet_sensuspro.c reefnet_sensuspro_parser.c \
	reefnet_sensusultra.h reefnet_sensusultra.c reefnet_sensusultra_parser.c \
	uwatec.h \
	uwatec_aladin.h uwatec_aladin.c \
	uwatec_memomouse.h uwatec_memomouse.c uwatec_memomouse_parser.c \
	uwatec_smart.h uwatec_smart.c uwatec_smart_parser.c \
	oceanic.h \
	oceanic_common.h oceanic_common.c \
	oceanic_atom2.h oceanic_atom2.c oceanic_atom2_parser.c \
	oceanic_veo250.h oceanic_veo250.c oceanic_veo250_parser.c \
	oceanic_vtpro.h oceanic_vtpro.c oceanic_vtpro_parser.c \
	mares.h \
	mares_common.h mares_common.c \
	mares_nemo.h mares_nemo.c mares_nemo_parser.c \
	mares_puck.h mares_puck.c \
	mares_darwin.h mares_darwin.c mares_darwin_parser.c \
	mares_iconhd.h mares_iconhd.c mares_iconhd_parser.c \
	hw.h \
	hw_ostc.h hw_ostc.c hw_ostc_parser.c \
	cressi.h \
	cressi_edy.h cressi_edy.c cressi_edy_parser.c \
	zeagle.h \
	zeagle_n2ition3.h zeagle_n2ition3.c \
	atomics.h \
	atomics_cobalt.h atomics_cobalt.c atomics_cobalt_parser.c \
	ringbuffer.h ringbuffer.c \
	checksum.h checksum.c \
	array.h array.c \
	buffer.h buffer.c \
	utils.h utils.c

if OS_WIN32
libdivecomputer_la_SOURCES += serial.h serial_win32.c
else
libdivecomputer_la_SOURCES += serial.h serial_posix.c
endif

if IRDA
if OS_WIN32
libdivecomputer_la_LIBADD += -lws2_32
endif
libdivecomputer_la_SOURCES += irda.h irda.c
else
libdivecomputer_la_SOURCES += irda.h irda_dummy.c
endif

if OS_WIN32
libdivecomputer_la_SOURCES += libdivecomputer.rc
endif

libdivecomputer_la_DEPENDENCIES = libdivecomputer.exp 

libdivecomputer.exp: libdivecomputer.symbols
	$(AM_V_GEN) sed -e '/^$$/d' $< > $@

.rc.lo:
	$(AM_V_GEN) $(LIBTOOL) --silent --tag=RC --mode=compile $(RC) $< -o $@

FORCE:
$(top_srcdir)/revision: FORCE
	@if (test -d $(top_srcdir)/.git && cd $(top_srcdir) \
	     && git rev-parse --verify HEAD) > revision-t 2>/dev/null \
	  && ! cmp -s revision-t $@; then \
	  mv -f revision-t $@; \
	else \
	  rm -f revision-t; \
	  if ! test -f $@; then touch $@; fi; \
	fi

revision.h: $(top_srcdir)/revision Makefile.am
	$(AM_V_GEN) echo "#define DC_VERSION_REVISION \""`cat $(top_srcdir)/revision`"\"" > $@

BUILT_SOURCES = revision.h
EXTRA_DIST = libdivecomputer.symbols $(top_srcdir)/revision
CLEANFILES = libdivecomputer.exp revision.h revision-t
