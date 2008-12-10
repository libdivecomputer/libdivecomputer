#
# Public header files.
#

libdivecomputerdir = $(includedir)/libdivecomputer
libdivecomputer_HEADERS = 	\
	utils.h \
	device.h \
	parser.h \
	units.h \
	suunto.h \
	suunto_eon.h \
	suunto_vyper.h \
	suunto_vyper2.h  \
	suunto_d9.h \
	reefnet.h \
	reefnet_sensuspro.h \
	reefnet_sensusultra.h \
	uwatec.h \
	uwatec_aladin.h \
	uwatec_memomouse.h \
	uwatec_smart.h \
	oceanic.h \
	oceanic_atom2.h \
	oceanic_veo250.h \
	oceanic_vtpro.h

#
# Source files.
#

lib_LTLIBRARIES = libdivecomputer.la

libdivecomputer_la_LDFLAGS = \
	-no-undefined \
	-export-symbols libdivecomputer.exp

libdivecomputer_la_SOURCES = \
	device.h device-private.h device.c \
	parser.h parser-private.h parser.c \
	suunto.h \
	suunto_common.h suunto_common.c \
	suunto_eon.h suunto_eon.c \
	suunto_vyper.h suunto_vyper.c suunto_vyper_parser.c suunto_spyder_parser.c \
	suunto_vyper2.h suunto_vyper2.c \
	suunto_d9.h suunto_d9.c suunto_d9_parser.c \
	reefnet.h \
	reefnet_sensuspro.h reefnet_sensuspro.c reefnet_sensuspro_parser.c \
	reefnet_sensusultra.h reefnet_sensusultra.c reefnet_sensusultra_parser.c \
	uwatec.h \
	uwatec_aladin.h uwatec_aladin.c \
	uwatec_memomouse.h uwatec_memomouse.c uwatec_memomouse_parser.c \
	oceanic.h \
	oceanic_atom2.h oceanic_atom2.c \
	oceanic_veo250.h oceanic_veo250.c \
	oceanic_vtpro.h oceanic_vtpro.c \
	ringbuffer.h ringbuffer.c \
	checksum.h checksum.c \
	array.h array.c \
	utils.h utils.c

if OS_WIN32
libdivecomputer_la_SOURCES += serial.h serial_win32.c
else
libdivecomputer_la_SOURCES += serial.h serial_posix.c
endif

if IRDA
if IRDA_WIN32
libdivecomputer_la_LIBADD = -lws2_32
endif
libdivecomputer_la_SOURCES += \
	irda.h irda.c \
	uwatec_smart.h uwatec_smart.c uwatec_smart_parser.c
endif

libdivecomputer_la_DEPENDENCIES = libdivecomputer.exp 

libdivecomputer.exp: libdivecomputer.symbols
if IRDA
	$(CPP) -P -DSERIAL -DIRDA - < $< | sed -e '/^$$/d' > $@
else
	$(CPP) -P -DSERIAL - < $< | sed -e '/^$$/d' > $@
endif

CLEANFILES = libdivecomputer.exp 
EXTRA_DIST = libdivecomputer.symbols
