AM_CFLAGS = -I$(top_srcdir)/src
LDADD = $(top_builddir)/src/libdivecomputer.la

bin_PROGRAMS = \
	eon \
	vyper \
	vyper2 \
	d9 \
	sensuspro \
	sensusultra \
	aladin \
	memomouse

if IRDA
bin_PROGRAMS += smart
endif

eon_SOURCES = suunto_eon_test.c

vyper_SOURCES = suunto_vyper_test.c

vyper2_SOURCES = suunto_vyper2_test.c

d9_SOURCES = suunto_d9_test.c

sensuspro_SOURCES = reefnet_sensuspro_test.c

sensusultra_SOURCES = reefnet_sensusultra_test.c

aladin_SOURCES = uwatec_aladin_test.c

memomouse_SOURCES = uwatec_memomouse_test.c

if IRDA
smart_SOURCES = uwatec_smart_test.c
endif
