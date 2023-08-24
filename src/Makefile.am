AM_CPPFLAGS = -I$(top_builddir)/include -I$(top_srcdir)/include
AM_CFLAGS = $(LIBUSB_CFLAGS) $(HIDAPI_CFLAGS) $(BLUEZ_CFLAGS)

lib_LTLIBRARIES = libdivecomputer.la

libdivecomputer_la_LIBADD = $(LIBUSB_LIBS) $(HIDAPI_LIBS) $(BLUEZ_LIBS) -lm
libdivecomputer_la_LDFLAGS = \
	-version-info $(DC_VERSION_LIBTOOL) \
	-no-undefined \
	-export-symbols libdivecomputer.exp

if OS_WIN32
libdivecomputer_la_LIBADD += -lws2_32
libdivecomputer_la_LDFLAGS += -Wc,-static-libgcc
endif

libdivecomputer_la_SOURCES = \
	version.c \
	descriptor.c \
	iostream-private.h iostream.c \
	iterator-private.h iterator.c \
	common-private.h common.c \
	context-private.h context.c \
	device-private.h device.c \
	parser-private.h parser.c \
	datetime.c \
	timer.h timer.c \
	suunto_common.h suunto_common.c \
	suunto_common2.h suunto_common2.c \
	suunto_solution.h suunto_solution.c suunto_solution_parser.c \
	suunto_eon.h suunto_eon.c suunto_eon_parser.c \
	suunto_vyper.h suunto_vyper.c suunto_vyper_parser.c \
	suunto_vyper2.h suunto_vyper2.c \
	suunto_d9.h suunto_d9.c suunto_d9_parser.c \
	suunto_eonsteel.h suunto_eonsteel.c suunto_eonsteel_parser.c \
	reefnet_sensus.h reefnet_sensus.c reefnet_sensus_parser.c \
	reefnet_sensuspro.h reefnet_sensuspro.c reefnet_sensuspro_parser.c \
	reefnet_sensusultra.h reefnet_sensusultra.c reefnet_sensusultra_parser.c \
	uwatec_aladin.h uwatec_aladin.c \
	uwatec_memomouse.h uwatec_memomouse.c uwatec_memomouse_parser.c \
	uwatec_smart.h uwatec_smart.c uwatec_smart_parser.c \
	oceanic_common.h oceanic_common.c \
	oceanic_atom2.h oceanic_atom2.c oceanic_atom2_parser.c \
	oceanic_veo250.h oceanic_veo250.c oceanic_veo250_parser.c \
	oceanic_vtpro.h oceanic_vtpro.c oceanic_vtpro_parser.c \
	mares_common.h mares_common.c \
	mares_nemo.h mares_nemo.c mares_nemo_parser.c \
	mares_puck.h mares_puck.c \
	mares_darwin.h mares_darwin.c mares_darwin_parser.c \
	mares_iconhd.h mares_iconhd.c mares_iconhd_parser.c \
	ihex.h ihex.c \
	hw_ostc.h hw_ostc.c hw_ostc_parser.c \
	hw_frog.h hw_frog.c \
	hw_ostc3.h hw_ostc3.c \
	aes.h aes.c \
	cressi_edy.h cressi_edy.c cressi_edy_parser.c \
	cressi_leonardo.h cressi_leonardo.c cressi_leonardo_parser.c \
	cressi_goa.h cressi_goa.c cressi_goa_parser.c \
	zeagle_n2ition3.h zeagle_n2ition3.c \
	atomics_cobalt.h atomics_cobalt.c atomics_cobalt_parser.c \
	shearwater_common.h shearwater_common.c \
	shearwater_predator.h shearwater_predator.c shearwater_predator_parser.c \
	shearwater_petrel.h shearwater_petrel.c \
	diverite_nitekq.h diverite_nitekq.c diverite_nitekq_parser.c \
	citizen_aqualand.h citizen_aqualand.c citizen_aqualand_parser.c \
	divesystem_idive.h divesystem_idive.c divesystem_idive_parser.c \
	platform.h platform.c \
	ringbuffer.h ringbuffer.c \
	rbstream.h rbstream.c \
	checksum.h checksum.c \
	array.h array.c \
	buffer.c \
	cochran_commander.h cochran_commander.c cochran_commander_parser.c \
	tecdiving_divecomputereu.h tecdiving_divecomputereu.c tecdiving_divecomputereu_parser.c \
	mclean_extreme.h mclean_extreme.c mclean_extreme_parser.c \
	liquivision_lynx.h liquivision_lynx.c liquivision_lynx_parser.c \
	sporasub_sp2.h sporasub_sp2.c sporasub_sp2_parser.c \
	deepsix_excursion.h deepsix_excursion.c deepsix_excursion_parser.c \
	seac_screen.h seac_screen.c seac_screen_parser.c \
	deepblu_cosmiq.h deepblu_cosmiq.c deepblu_cosmiq_parser.c \
	oceans_s1_common.h oceans_s1_common.c \
	oceans_s1.h oceans_s1.c oceans_s1_parser.c \
	divesoft_freedom.h divesoft_freedom.c divesoft_freedom_parser.c \
	hdlc.h hdlc.c \
	packet.h packet.c \
	socket.h socket.c \
	irda.c \
	usb.c \
	usbhid.c \
	bluetooth.c \
	custom.c

if OS_WIN32
libdivecomputer_la_SOURCES += serial_win32.c
else
libdivecomputer_la_SOURCES += serial_posix.c
endif

if OS_WIN32
libdivecomputer_la_SOURCES += libdivecomputer.rc
endif

libdivecomputer_la_DEPENDENCIES = libdivecomputer.exp

libdivecomputer.exp: libdivecomputer.symbols
	$(AM_V_GEN) sed -e '/^$$/d' $< > $@

.rc.lo:
	$(AM_V_GEN) $(LIBTOOL) --silent --tag=CC --mode=compile $(RC) $(DEFS) $(DEFAULT_INCLUDES) $(AM_CPPFLAGS) $< -o $@

libdivecomputer.lo: revision.h

FORCE:
$(top_srcdir)/revision: FORCE
	@if (test -e $(top_srcdir)/.git && cd $(top_srcdir) \
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
