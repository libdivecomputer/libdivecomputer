AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_builddir)/include
LDADD = $(top_builddir)/src/libdivecomputer.la

bin_PROGRAMS = \
	universal \
	solution \
	eon \
	vyper \
	vyper2 \
	d9 \
	sensus \
	sensuspro \
	sensusultra \
	aladin \
	memomouse \
	atom2 \
	veo250 \
	vtpro \
	nemo \
	puck \
	darwin \
	iconhd \
	ostc \
	frog \
	edy \
	n2ition3

if IRDA
bin_PROGRAMS += smart
endif

COMMON = common.c common.h \
	utils.c utils.h

universal_SOURCES = universal.c $(COMMON)

solution_SOURCES = suunto_solution_test.c $(COMMON)

eon_SOURCES = suunto_eon_test.c $(COMMON)

vyper_SOURCES = suunto_vyper_test.c $(COMMON)

vyper2_SOURCES = suunto_vyper2_test.c $(COMMON)

d9_SOURCES = suunto_d9_test.c $(COMMON)

sensus_SOURCES = reefnet_sensus_test.c $(COMMON)

sensuspro_SOURCES = reefnet_sensuspro_test.c $(COMMON)

sensusultra_SOURCES = reefnet_sensusultra_test.c $(COMMON)

aladin_SOURCES = uwatec_aladin_test.c $(COMMON)

memomouse_SOURCES = uwatec_memomouse_test.c $(COMMON)

atom2_SOURCES = oceanic_atom2_test.c $(COMMON)

veo250_SOURCES = oceanic_veo250_test.c $(COMMON)

vtpro_SOURCES = oceanic_vtpro_test.c $(COMMON)

nemo_SOURCES = mares_nemo_test.c $(COMMON)

puck_SOURCES = mares_puck_test.c $(COMMON)

darwin_SOURCES = mares_darwin_test.c $(COMMON)

iconhd_SOURCES = mares_iconhd_test.c $(COMMON)

ostc_SOURCES = hw_ostc_test.c $(COMMON)

frog_SOURCES = hw_frog_test.c $(COMMON)

edy_SOURCES = cressi_edy_test.c $(COMMON)

n2ition3_SOURCES = zeagle_n2ition3_test.c $(COMMON)

if IRDA
smart_SOURCES = uwatec_smart_test.c $(COMMON)
endif
