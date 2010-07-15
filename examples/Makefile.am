AM_CFLAGS = -I$(top_srcdir)/src
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
	iconhd \
	ostc \
	edy \
	n2ition3

if IRDA
bin_PROGRAMS += smart
endif

universal_SOURCES = universal.c

solution_SOURCES = suunto_solution_test.c

eon_SOURCES = suunto_eon_test.c

vyper_SOURCES = suunto_vyper_test.c

vyper2_SOURCES = suunto_vyper2_test.c

d9_SOURCES = suunto_d9_test.c

sensus_SOURCES = reefnet_sensus_test.c

sensuspro_SOURCES = reefnet_sensuspro_test.c

sensusultra_SOURCES = reefnet_sensusultra_test.c

aladin_SOURCES = uwatec_aladin_test.c

memomouse_SOURCES = uwatec_memomouse_test.c

atom2_SOURCES = oceanic_atom2_test.c

veo250_SOURCES = oceanic_veo250_test.c

vtpro_SOURCES = oceanic_vtpro_test.c

nemo_SOURCES = mares_nemo_test.c

puck_SOURCES = mares_puck_test.c

iconhd_SOURCES = mares_iconhd_test.c

ostc_SOURCES = hw_ostc_test.c

edy_SOURCES = cressi_edy_test.c

n2ition3_SOURCES = zeagle_n2ition3_test.c

if IRDA
smart_SOURCES = uwatec_smart_test.c
endif
