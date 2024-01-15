/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/usbhid.h>
#include <libdivecomputer/usb.h>

#include "iterator-private.h"
#include "platform.h"

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))
#define C_ARRAY_ITEMSIZE(array) (sizeof *(array))

#define DC_FILTER_INTERNAL(key, values, isnullterminated, match) \
	dc_filter_internal( \
		key, \
		values, \
		C_ARRAY_SIZE(values) - isnullterminated, \
		C_ARRAY_ITEMSIZE(values), \
		match)

typedef int (*dc_match_t)(const void *, const void *);

typedef int (*dc_filter_t) (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);

static int dc_filter_uwatec (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_suunto (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_shearwater (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_hw (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_tecdiving (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_mares (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_divesystem (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_oceanic (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_mclean (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_atomic (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_deepsix (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_deepblu (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_oceans (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);
static int dc_filter_divesoft (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);

static dc_status_t dc_descriptor_iterator_next (dc_iterator_t *iterator, void *item);

struct dc_descriptor_t {
	const char *vendor;
	const char *product;
	dc_family_t type;
	unsigned int model;
	unsigned int transports;
	dc_filter_t filter;
};

typedef struct dc_descriptor_iterator_t {
	dc_iterator_t base;
	size_t current;
} dc_descriptor_iterator_t;

static const dc_iterator_vtable_t dc_descriptor_iterator_vtable = {
	sizeof(dc_descriptor_iterator_t),
	dc_descriptor_iterator_next,
	NULL,
};

/*
 * The model numbers in the table are the actual model numbers reported by the
 * device. For devices where there is no model number available (or known), an
 * artifical number (starting at zero) is assigned.  If the model number isn't
 * actually used to identify individual models, identical values are assigned.
 */

static const dc_descriptor_t g_descriptors[] = {
	/* Suunto Solution */
	{"Suunto", "Solution", DC_FAMILY_SUUNTO_SOLUTION, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Suunto Eon */
	{"Suunto", "Eon",             DC_FAMILY_SUUNTO_EON, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Solution Alpha",  DC_FAMILY_SUUNTO_EON, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Solution Nitrox", DC_FAMILY_SUUNTO_EON, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Suunto Vyper */
	{"Suunto", "Spyder",   DC_FAMILY_SUUNTO_VYPER, 0x01, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Stinger",  DC_FAMILY_SUUNTO_VYPER, 0x03, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Mosquito", DC_FAMILY_SUUNTO_VYPER, 0x04, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D3",       DC_FAMILY_SUUNTO_VYPER, 0x05, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Vyper",    DC_FAMILY_SUUNTO_VYPER, 0x0A, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Vytec",    DC_FAMILY_SUUNTO_VYPER, 0X0B, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Cobra",    DC_FAMILY_SUUNTO_VYPER, 0X0C, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Gekko",    DC_FAMILY_SUUNTO_VYPER, 0X0D, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Zoop",     DC_FAMILY_SUUNTO_VYPER, 0x16, DC_TRANSPORT_SERIAL, NULL},
	/* Suunto Vyper 2 */
	{"Suunto", "Vyper 2",   DC_FAMILY_SUUNTO_VYPER2, 0x10, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Cobra 2",   DC_FAMILY_SUUNTO_VYPER2, 0x11, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Vyper Air", DC_FAMILY_SUUNTO_VYPER2, 0x13, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Cobra 3",   DC_FAMILY_SUUNTO_VYPER2, 0x14, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "HelO2",     DC_FAMILY_SUUNTO_VYPER2, 0x15, DC_TRANSPORT_SERIAL, NULL},
	/* Suunto D9 */
	{"Suunto", "D9",         DC_FAMILY_SUUNTO_D9, 0x0E, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D6",         DC_FAMILY_SUUNTO_D9, 0x0F, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D4",         DC_FAMILY_SUUNTO_D9, 0x12, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D4i",        DC_FAMILY_SUUNTO_D9, 0x19, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D6i",        DC_FAMILY_SUUNTO_D9, 0x1A, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D9tx",       DC_FAMILY_SUUNTO_D9, 0x1B, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "DX",         DC_FAMILY_SUUNTO_D9, 0x1C, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Vyper Novo", DC_FAMILY_SUUNTO_D9, 0x1D, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Zoop Novo",  DC_FAMILY_SUUNTO_D9, 0x1E, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "Zoop Novo",  DC_FAMILY_SUUNTO_D9, 0x1F, DC_TRANSPORT_SERIAL, NULL},
	{"Suunto", "D4f",        DC_FAMILY_SUUNTO_D9, 0x20, DC_TRANSPORT_SERIAL, NULL},
	/* Suunto EON Steel */
	{"Suunto", "EON Steel",       DC_FAMILY_SUUNTO_EONSTEEL, 0, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_suunto},
	{"Suunto", "EON Core",        DC_FAMILY_SUUNTO_EONSTEEL, 1, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_suunto},
	{"Suunto", "D5",              DC_FAMILY_SUUNTO_EONSTEEL, 2, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_suunto},
	{"Suunto", "EON Steel Black", DC_FAMILY_SUUNTO_EONSTEEL, 3, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_suunto},
	/* Uwatec Aladin */
	{"Uwatec", "Aladin Air Twin",     DC_FAMILY_UWATEC_ALADIN, 0x1C, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Sport Plus",   DC_FAMILY_UWATEC_ALADIN, 0x3E, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Pro",          DC_FAMILY_UWATEC_ALADIN, 0x3F, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Air Z",        DC_FAMILY_UWATEC_ALADIN, 0x44, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Air Z O2",     DC_FAMILY_UWATEC_ALADIN, 0xA4, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Air Z Nitrox", DC_FAMILY_UWATEC_ALADIN, 0xF4, DC_TRANSPORT_SERIAL, NULL},
	{"Uwatec", "Aladin Pro Ultra",    DC_FAMILY_UWATEC_ALADIN, 0xFF, DC_TRANSPORT_SERIAL, NULL},
	/* Uwatec Memomouse */
	{"Uwatec", "Memomouse", DC_FAMILY_UWATEC_MEMOMOUSE, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Uwatec Smart */
	{"Uwatec",   "Smart Pro",           DC_FAMILY_UWATEC_SMART, 0x10, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Galileo Sol",         DC_FAMILY_UWATEC_SMART, 0x11, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Galileo Luna",        DC_FAMILY_UWATEC_SMART, 0x11, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Galileo Terra",       DC_FAMILY_UWATEC_SMART, 0x11, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin Tec",          DC_FAMILY_UWATEC_SMART, 0x12, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin Prime",        DC_FAMILY_UWATEC_SMART, 0x12, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin Tec 2G",       DC_FAMILY_UWATEC_SMART, 0x13, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin 2G",           DC_FAMILY_UWATEC_SMART, 0x13, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Subgear",  "XP-10",               DC_FAMILY_UWATEC_SMART, 0x13, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Smart Com",           DC_FAMILY_UWATEC_SMART, 0x14, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin 2G",           DC_FAMILY_UWATEC_SMART, 0x15, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin Tec 3G",       DC_FAMILY_UWATEC_SMART, 0x15, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Aladin Sport",        DC_FAMILY_UWATEC_SMART, 0x15, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Subgear",  "XP-3G",               DC_FAMILY_UWATEC_SMART, 0x15, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Scubapro", "Aladin Sport Matrix", DC_FAMILY_UWATEC_SMART, 0x17, DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "Aladin H Matrix",     DC_FAMILY_UWATEC_SMART, 0x17, DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Uwatec",   "Smart Tec",           DC_FAMILY_UWATEC_SMART, 0x18, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Galileo Trimix",      DC_FAMILY_UWATEC_SMART, 0x19, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Uwatec",   "Smart Z",             DC_FAMILY_UWATEC_SMART, 0x1C, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Subgear",  "XP Air",              DC_FAMILY_UWATEC_SMART, 0x1C, DC_TRANSPORT_IRDA, dc_filter_uwatec},
	{"Scubapro", "Meridian",            DC_FAMILY_UWATEC_SMART, 0x20, DC_TRANSPORT_SERIAL, NULL},
	{"Scubapro", "Mantis",              DC_FAMILY_UWATEC_SMART, 0x20, DC_TRANSPORT_SERIAL, NULL},
	{"Scubapro", "Aladin Square",       DC_FAMILY_UWATEC_SMART, 0x22, DC_TRANSPORT_USBHID, dc_filter_uwatec},
	{"Scubapro", "Chromis",             DC_FAMILY_UWATEC_SMART, 0x24, DC_TRANSPORT_SERIAL, NULL},
	{"Scubapro", "Aladin A1",           DC_FAMILY_UWATEC_SMART, 0x25, DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "Mantis 2",            DC_FAMILY_UWATEC_SMART, 0x26, DC_TRANSPORT_SERIAL, NULL},
	{"Scubapro", "Aladin A2",           DC_FAMILY_UWATEC_SMART, 0x28, DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "G2 TEK",              DC_FAMILY_UWATEC_SMART, 0x31, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "G2",                  DC_FAMILY_UWATEC_SMART, 0x32, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "G2 Console",          DC_FAMILY_UWATEC_SMART, 0x32, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "G3",                  DC_FAMILY_UWATEC_SMART, 0x34, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "G2 HUD",              DC_FAMILY_UWATEC_SMART, 0x42, DC_TRANSPORT_USBHID | DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "Luna 2.0 AI",         DC_FAMILY_UWATEC_SMART, 0x50, DC_TRANSPORT_BLE, dc_filter_uwatec},
	{"Scubapro", "Luna 2.0",            DC_FAMILY_UWATEC_SMART, 0x51, DC_TRANSPORT_BLE, dc_filter_uwatec},
	/* Reefnet */
	{"Reefnet", "Sensus",       DC_FAMILY_REEFNET_SENSUS, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Reefnet", "Sensus Pro",   DC_FAMILY_REEFNET_SENSUSPRO, 2, DC_TRANSPORT_SERIAL, NULL},
	{"Reefnet", "Sensus Ultra", DC_FAMILY_REEFNET_SENSUSULTRA, 3, DC_TRANSPORT_SERIAL, NULL},
	/* Oceanic VT Pro */
	{"Aeris",    "500 AI",     DC_FAMILY_OCEANIC_VTPRO, 0x4151, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Versa Pro",  DC_FAMILY_OCEANIC_VTPRO, 0x4155, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Atmos 2",    DC_FAMILY_OCEANIC_VTPRO, 0x4158, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Pro Plus 2", DC_FAMILY_OCEANIC_VTPRO, 0x4159, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Atmos AI",   DC_FAMILY_OCEANIC_VTPRO, 0x4244, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "VT Pro",     DC_FAMILY_OCEANIC_VTPRO, 0x4245, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Wisdom",     DC_FAMILY_OCEANIC_VTPRO, 0x4246, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Elite",      DC_FAMILY_OCEANIC_VTPRO, 0x424F, DC_TRANSPORT_SERIAL, NULL},
	/* Oceanic Veo 250 */
	{"Genesis", "React Pro", DC_FAMILY_OCEANIC_VEO250, 0x4247, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic", "Veo 200",   DC_FAMILY_OCEANIC_VEO250, 0x424B, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic", "Veo 250",   DC_FAMILY_OCEANIC_VEO250, 0x424C, DC_TRANSPORT_SERIAL, NULL},
	{"Seemann", "XP5",       DC_FAMILY_OCEANIC_VEO250, 0x4251, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic", "Veo 180",   DC_FAMILY_OCEANIC_VEO250, 0x4252, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",   "XR-2",      DC_FAMILY_OCEANIC_VEO250, 0x4255, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Insight",  DC_FAMILY_OCEANIC_VEO250, 0x425A, DC_TRANSPORT_SERIAL, NULL},
	{"Hollis",  "DG02",      DC_FAMILY_OCEANIC_VEO250, 0x4352, DC_TRANSPORT_SERIAL, NULL},
	/* Oceanic Atom 2.0 */
	{"Oceanic",  "Atom 1.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4250, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4257, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "VT3",                 DC_FAMILY_OCEANIC_ATOM2, 0x4258, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4259, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Atom 2.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4342, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Geo",                 DC_FAMILY_OCEANIC_ATOM2, 0x4344, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Manta",               DC_FAMILY_OCEANIC_ATOM2, 0x4345, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "XR-1 NX",             DC_FAMILY_OCEANIC_ATOM2, 0x4346, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Datamask",            DC_FAMILY_OCEANIC_ATOM2, 0x4347, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Compumask",           DC_FAMILY_OCEANIC_ATOM2, 0x4348, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "F10",                 DC_FAMILY_OCEANIC_ATOM2, 0x434D, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x434E, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Wisdom 2",            DC_FAMILY_OCEANIC_ATOM2, 0x4350, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Insight 2",           DC_FAMILY_OCEANIC_ATOM2, 0x4353, DC_TRANSPORT_SERIAL, NULL},
	{"Genesis",  "React Pro White",     DC_FAMILY_OCEANIC_ATOM2, 0x4354, DC_TRANSPORT_SERIAL, NULL},
	{"Tusa",     "Element II (IQ-750)", DC_FAMILY_OCEANIC_ATOM2, 0x4357, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Veo 1.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4358, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Veo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4359, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Veo 3.0",             DC_FAMILY_OCEANIC_ATOM2, 0x435A, DC_TRANSPORT_SERIAL, NULL},
	{"Tusa",     "Zen (IQ-900)",        DC_FAMILY_OCEANIC_ATOM2, 0x4441, DC_TRANSPORT_SERIAL, NULL},
	{"Tusa",     "Zen Air (IQ-950)",    DC_FAMILY_OCEANIC_ATOM2, 0x4442, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Atmos AI 2",          DC_FAMILY_OCEANIC_ATOM2, 0x4443, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Pro Plus 2.1",        DC_FAMILY_OCEANIC_ATOM2, 0x4444, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Geo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4446, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "VT4",                 DC_FAMILY_OCEANIC_ATOM2, 0x4447, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4449, DC_TRANSPORT_SERIAL, NULL},
	{"Beuchat",  "Voyager 2G",          DC_FAMILY_OCEANIC_ATOM2, 0x444B, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Atom 3.0",            DC_FAMILY_OCEANIC_ATOM2, 0x444C, DC_TRANSPORT_SERIAL, NULL},
	{"Hollis",   "DG03",                DC_FAMILY_OCEANIC_ATOM2, 0x444D, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "OCS",                 DC_FAMILY_OCEANIC_ATOM2, 0x4450, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4451, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "VT 4.1",              DC_FAMILY_OCEANIC_ATOM2, 0x4452, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4453, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4455, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Atom 3.1",            DC_FAMILY_OCEANIC_ATOM2, 0x4456, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "A300 AI",             DC_FAMILY_OCEANIC_ATOM2, 0x4457, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Wisdom 3",            DC_FAMILY_OCEANIC_ATOM2, 0x4458, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "A300",                DC_FAMILY_OCEANIC_ATOM2, 0x445A, DC_TRANSPORT_SERIAL, NULL},
	{"Hollis",   "TX1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4542, DC_TRANSPORT_SERIAL, NULL},
	{"Beuchat",  "Mundial 2",           DC_FAMILY_OCEANIC_ATOM2, 0x4543, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Amphos",              DC_FAMILY_OCEANIC_ATOM2, 0x4545, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Amphos Air",          DC_FAMILY_OCEANIC_ATOM2, 0x4546, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Pro Plus 3",          DC_FAMILY_OCEANIC_ATOM2, 0x4548, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4549, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "OCi",                 DC_FAMILY_OCEANIC_ATOM2, 0x454B, DC_TRANSPORT_SERIAL, NULL},
	{"Aeris",    "A300CS",              DC_FAMILY_OCEANIC_ATOM2, 0x454C, DC_TRANSPORT_SERIAL, NULL},
	{"Tusa",     "Talis",               DC_FAMILY_OCEANIC_ATOM2, 0x454E, DC_TRANSPORT_SERIAL, NULL},
	{"Beuchat",  "Mundial 3",           DC_FAMILY_OCEANIC_ATOM2, 0x4550, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "Pro Plus X",          DC_FAMILY_OCEANIC_ATOM2, 0x4552, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Oceanic",  "F10",                 DC_FAMILY_OCEANIC_ATOM2, 0x4553, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4554, DC_TRANSPORT_SERIAL, NULL},
	{"Subgear",  "XP-Air",              DC_FAMILY_OCEANIC_ATOM2, 0x4555, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Vision",              DC_FAMILY_OCEANIC_ATOM2, 0x4556, DC_TRANSPORT_SERIAL, NULL},
	{"Oceanic",  "VTX",                 DC_FAMILY_OCEANIC_ATOM2, 0x4557, DC_TRANSPORT_SERIAL, NULL},
	{"Aqualung", "i300",                DC_FAMILY_OCEANIC_ATOM2, 0x4559, DC_TRANSPORT_SERIAL, NULL},
	{"Aqualung", "i750TC",              DC_FAMILY_OCEANIC_ATOM2, 0x455A, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i450T",               DC_FAMILY_OCEANIC_ATOM2, 0x4641, DC_TRANSPORT_SERIAL, NULL},
	{"Aqualung", "i550",                DC_FAMILY_OCEANIC_ATOM2, 0x4642, DC_TRANSPORT_SERIAL, NULL},
	{"Aqualung", "i200",                DC_FAMILY_OCEANIC_ATOM2, 0x4646, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Sage",                DC_FAMILY_OCEANIC_ATOM2, 0x4647, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i300C",               DC_FAMILY_OCEANIC_ATOM2, 0x4648, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i200C",               DC_FAMILY_OCEANIC_ATOM2, 0x4649, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i100",                DC_FAMILY_OCEANIC_ATOM2, 0x464E, DC_TRANSPORT_SERIAL, NULL},
	{"Aqualung", "i770R",               DC_FAMILY_OCEANIC_ATOM2, 0x4651, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i550C",               DC_FAMILY_OCEANIC_ATOM2, 0x4652, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Oceanic",  "Geo 4.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4653, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Oceanic",  "Veo 4.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4654, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Sherwood", "Wisdom 4",            DC_FAMILY_OCEANIC_ATOM2, 0x4655, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Oceanic",  "Pro Plus 4",          DC_FAMILY_OCEANIC_ATOM2, 0x4656, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Sherwood", "Amphos 2.0",          DC_FAMILY_OCEANIC_ATOM2, 0x4657, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Amphos Air 2.0",      DC_FAMILY_OCEANIC_ATOM2, 0x4658, DC_TRANSPORT_SERIAL, NULL},
	{"Sherwood", "Beacon",              DC_FAMILY_OCEANIC_ATOM2, 0x4742, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i470TC",              DC_FAMILY_OCEANIC_ATOM2, 0x4743, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Aqualung", "i200C",               DC_FAMILY_OCEANIC_ATOM2, 0x4749, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	{"Oceanic",  "Geo Air",             DC_FAMILY_OCEANIC_ATOM2, 0x474B, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_oceanic},
	/* Mares Nemo */
	{"Mares", "Nemo",         DC_FAMILY_MARES_NEMO, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Steel",   DC_FAMILY_MARES_NEMO, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Titanium",DC_FAMILY_MARES_NEMO, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Excel",   DC_FAMILY_MARES_NEMO, 17, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Apneist", DC_FAMILY_MARES_NEMO, 18, DC_TRANSPORT_SERIAL, NULL},
	/* Mares Puck */
	{"Mares", "Puck",      DC_FAMILY_MARES_PUCK, 7, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Puck Air",  DC_FAMILY_MARES_PUCK, 19, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Air",  DC_FAMILY_MARES_PUCK, 4, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Nemo Wide", DC_FAMILY_MARES_PUCK, 1, DC_TRANSPORT_SERIAL, NULL},
	/* Mares Darwin */
	{"Mares", "Darwin",     DC_FAMILY_MARES_DARWIN , 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "M1",         DC_FAMILY_MARES_DARWIN , 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "M2",         DC_FAMILY_MARES_DARWIN , 0, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Darwin Air", DC_FAMILY_MARES_DARWIN , 1, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Airlab",     DC_FAMILY_MARES_DARWIN , 1, DC_TRANSPORT_SERIAL, NULL},
	/* Mares Icon HD */
	{"Mares", "Matrix",            DC_FAMILY_MARES_ICONHD , 0x0F, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Smart",             DC_FAMILY_MARES_ICONHD , 0x000010, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Smart Apnea",       DC_FAMILY_MARES_ICONHD , 0x010010, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Icon HD",           DC_FAMILY_MARES_ICONHD , 0x14, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Icon HD Net Ready", DC_FAMILY_MARES_ICONHD , 0x15, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Puck Pro",          DC_FAMILY_MARES_ICONHD , 0x18, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Puck Pro +",        DC_FAMILY_MARES_ICONHD , 0x18, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Nemo Wide 2",       DC_FAMILY_MARES_ICONHD , 0x19, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Genius",            DC_FAMILY_MARES_ICONHD , 0x1C, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Puck 2",            DC_FAMILY_MARES_ICONHD , 0x1F, DC_TRANSPORT_SERIAL, NULL},
	{"Mares", "Quad Air",          DC_FAMILY_MARES_ICONHD , 0x23, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Smart Air",         DC_FAMILY_MARES_ICONHD , 0x24, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Quad",              DC_FAMILY_MARES_ICONHD , 0x29, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_mares},
	{"Mares", "Horizon",           DC_FAMILY_MARES_ICONHD , 0x2C, DC_TRANSPORT_SERIAL, NULL},
	/* Heinrichs Weikamp */
	{"Heinrichs Weikamp", "OSTC",     DC_FAMILY_HW_OSTC, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC Mk2", DC_FAMILY_HW_OSTC, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC 2N",  DC_FAMILY_HW_OSTC, 2, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC 2C",  DC_FAMILY_HW_OSTC, 3, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "Frog",     DC_FAMILY_HW_FROG, 0, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x11, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x13, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x1B, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 3",     DC_FAMILY_HW_OSTC3, 0x0A, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC Plus",  DC_FAMILY_HW_OSTC3, 0x13, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC Plus",  DC_FAMILY_HW_OSTC3, 0x1A, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 4",     DC_FAMILY_HW_OSTC3, 0x3B, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC cR",    DC_FAMILY_HW_OSTC3, 0x05, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC cR",    DC_FAMILY_HW_OSTC3, 0x07, DC_TRANSPORT_SERIAL, NULL},
	{"Heinrichs Weikamp", "OSTC Sport", DC_FAMILY_HW_OSTC3, 0x12, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC Sport", DC_FAMILY_HW_OSTC3, 0x13, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	{"Heinrichs Weikamp", "OSTC 2 TR",  DC_FAMILY_HW_OSTC3, 0x33, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_hw},
	/* Cressi Edy */
	{"Tusa",   "IQ-700", DC_FAMILY_CRESSI_EDY, 0x05, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Edy",    DC_FAMILY_CRESSI_EDY, 0x08, DC_TRANSPORT_SERIAL, NULL},
	/* Cressi Leonardo */
	{"Cressi", "Leonardo", DC_FAMILY_CRESSI_LEONARDO, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Giotto",   DC_FAMILY_CRESSI_LEONARDO, 4, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Newton",   DC_FAMILY_CRESSI_LEONARDO, 5, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Drake",    DC_FAMILY_CRESSI_LEONARDO, 6, DC_TRANSPORT_SERIAL, NULL},
	/* Cressi Goa */
	{"Cressi", "Cartesio", DC_FAMILY_CRESSI_GOA, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Goa",      DC_FAMILY_CRESSI_GOA, 2, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Donatello",    DC_FAMILY_CRESSI_GOA, 4, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Michelangelo", DC_FAMILY_CRESSI_GOA, 5, DC_TRANSPORT_SERIAL, NULL},
	{"Cressi", "Neon",     DC_FAMILY_CRESSI_GOA, 9, DC_TRANSPORT_SERIAL, NULL},
	/* Zeagle N2iTiON3 */
	{"Zeagle",    "N2iTiON3",   DC_FAMILY_ZEAGLE_N2ITION3, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Apeks",     "Quantum X",  DC_FAMILY_ZEAGLE_N2ITION3, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Dive Rite", "NiTek Trio", DC_FAMILY_ZEAGLE_N2ITION3, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Scubapro",  "XTender 5",  DC_FAMILY_ZEAGLE_N2ITION3, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Atomic Aquatics Cobalt */
	{"Atomic Aquatics", "Cobalt",   DC_FAMILY_ATOMICS_COBALT, 0, DC_TRANSPORT_USB, dc_filter_atomic},
	{"Atomic Aquatics", "Cobalt 2", DC_FAMILY_ATOMICS_COBALT, 2, DC_TRANSPORT_USB, dc_filter_atomic},
	/* Shearwater Predator */
	{"Shearwater", "Predator", DC_FAMILY_SHEARWATER_PREDATOR, 2, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_shearwater},
	/* Shearwater Petrel */
	{"Shearwater", "Petrel",    DC_FAMILY_SHEARWATER_PETREL, 3, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_shearwater},
	{"Shearwater", "Petrel 2",  DC_FAMILY_SHEARWATER_PETREL, 3, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Nerd",      DC_FAMILY_SHEARWATER_PETREL, 4, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_shearwater},
	{"Shearwater", "Perdix",    DC_FAMILY_SHEARWATER_PETREL, 5, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Perdix AI", DC_FAMILY_SHEARWATER_PETREL, 6, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Nerd 2",    DC_FAMILY_SHEARWATER_PETREL, 7, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Teric",     DC_FAMILY_SHEARWATER_PETREL, 8, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Peregrine", DC_FAMILY_SHEARWATER_PETREL, 9, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Petrel 3",  DC_FAMILY_SHEARWATER_PETREL, 10, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Perdix 2",  DC_FAMILY_SHEARWATER_PETREL, 11, DC_TRANSPORT_BLE, dc_filter_shearwater},
	{"Shearwater", "Tern",      DC_FAMILY_SHEARWATER_PETREL, 12, DC_TRANSPORT_BLE, dc_filter_shearwater},
	/* Dive Rite NiTek Q */
	{"Dive Rite", "NiTek Q",   DC_FAMILY_DIVERITE_NITEKQ, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Citizen Hyper Aqualand */
	{"Citizen", "Hyper Aqualand", DC_FAMILY_CITIZEN_AQUALAND, 0, DC_TRANSPORT_SERIAL, NULL},
	/* DiveSystem/Ratio iDive */
	{"DiveSystem", "Orca",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x02, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Pro",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x03, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive DAN",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x04, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Tech",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x05, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Reb",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x06, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Stealth", DC_FAMILY_DIVESYSTEM_IDIVE, 0x07, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Free",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x08, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Easy",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x09, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive X3M",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x0A, DC_TRANSPORT_SERIAL, NULL},
	{"DiveSystem", "iDive Deep",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x0B, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M GPS Pro ", DC_FAMILY_DIVESYSTEM_IDIVE, 0x21, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M GPS Easy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x22, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M GPS Deep", DC_FAMILY_DIVESYSTEM_IDIVE, 0x23, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M GPS Tech+",DC_FAMILY_DIVESYSTEM_IDIVE, 0x24, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M GPS Reb",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x25, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M GPS Fancy",DC_FAMILY_DIVESYSTEM_IDIVE, 0x26, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_divesystem},
	{"Ratio",      "iX3M Pro Fancy",DC_FAMILY_DIVESYSTEM_IDIVE, 0x31, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M Pro Easy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x32, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M Pro Pro",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x33, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M Pro Deep", DC_FAMILY_DIVESYSTEM_IDIVE, 0x34, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M Pro Tech+",DC_FAMILY_DIVESYSTEM_IDIVE, 0x35, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M Pro Reb",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x36, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Free",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x40, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Fancy",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x41, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Easy",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x42, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Pro",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x43, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Deep",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x44, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Tech+",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x45, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Reb",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x46, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Free", DC_FAMILY_DIVESYSTEM_IDIVE, 0x50, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Fancy",DC_FAMILY_DIVESYSTEM_IDIVE, 0x51, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Easy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x52, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Pro",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x53, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Deep", DC_FAMILY_DIVESYSTEM_IDIVE, 0x54, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Tech+",DC_FAMILY_DIVESYSTEM_IDIVE, 0x55, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive Color Reb",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x56, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 GPS Fancy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x60, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 GPS Easy",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x61, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 GPS Pro ",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x62, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 GPS Deep",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x63, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 GPS Tech+", DC_FAMILY_DIVESYSTEM_IDIVE, 0x64, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 GPS Reb",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x65, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2021 Pro Fancy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x70, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 Pro Easy",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x71, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 Pro Pro",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x72, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 Pro Deep",  DC_FAMILY_DIVESYSTEM_IDIVE, 0x73, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 Pro Tech+", DC_FAMILY_DIVESYSTEM_IDIVE, 0x74, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iX3M 2021 Pro Reb",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x75, DC_TRANSPORT_SERIAL, NULL},
	{"Ratio",      "iDive 2 Free",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x80, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Fancy",       DC_FAMILY_DIVESYSTEM_IDIVE, 0x81, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Easy",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x82, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Pro",         DC_FAMILY_DIVESYSTEM_IDIVE, 0x83, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Deep",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x84, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Tech",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x85, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iDive 2 Reb",         DC_FAMILY_DIVESYSTEM_IDIVE, 0x86, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Gauge",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x90, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Easy",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x91, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Pro",      DC_FAMILY_DIVESYSTEM_IDIVE, 0x92, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Deep",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x93, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Tech",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x94, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 GPS Reb",      DC_FAMILY_DIVESYSTEM_IDIVE, 0x95, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "ATOM",                DC_FAMILY_DIVESYSTEM_IDIVE, 0x96, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 Gauge",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x100, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 Easy",         DC_FAMILY_DIVESYSTEM_IDIVE, 0x101, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 Pro",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x102, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 Deep",         DC_FAMILY_DIVESYSTEM_IDIVE, 0x103, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Ratio",      "iX3M 2 Tech+",        DC_FAMILY_DIVESYSTEM_IDIVE, 0x104, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLE, dc_filter_divesystem},
	{"Seac",       "Jack",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x1000, DC_TRANSPORT_SERIAL, NULL},
	{"Seac",       "Guru",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x1002, DC_TRANSPORT_SERIAL, NULL},
	/* Cochran Commander */
	{"Cochran", "Commander TM", DC_FAMILY_COCHRAN_COMMANDER, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Cochran", "Commander I",  DC_FAMILY_COCHRAN_COMMANDER, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Cochran", "Commander II", DC_FAMILY_COCHRAN_COMMANDER, 2, DC_TRANSPORT_SERIAL, NULL},
	{"Cochran", "EMC-14",       DC_FAMILY_COCHRAN_COMMANDER, 3, DC_TRANSPORT_SERIAL, NULL},
	{"Cochran", "EMC-16",       DC_FAMILY_COCHRAN_COMMANDER, 4, DC_TRANSPORT_SERIAL, NULL},
	{"Cochran", "EMC-20H",      DC_FAMILY_COCHRAN_COMMANDER, 5, DC_TRANSPORT_SERIAL, NULL},
	/* Tecdiving DiveComputer.eu */
	{"Tecdiving", "DiveComputer.eu", DC_FAMILY_TECDIVING_DIVECOMPUTEREU, 0, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH, dc_filter_tecdiving},
	/* McLean Extreme */
	{ "McLean", "Extreme", DC_FAMILY_MCLEAN_EXTREME, 0, DC_TRANSPORT_SERIAL | DC_TRANSPORT_BLUETOOTH | DC_TRANSPORT_BLE, dc_filter_mclean},
	/* Liquivision */
	{"Liquivision", "Xen",  DC_FAMILY_LIQUIVISION_LYNX, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Liquivision", "Xeo",  DC_FAMILY_LIQUIVISION_LYNX, 1, DC_TRANSPORT_SERIAL, NULL},
	{"Liquivision", "Lynx", DC_FAMILY_LIQUIVISION_LYNX, 2, DC_TRANSPORT_SERIAL, NULL},
	{"Liquivision", "Kaon", DC_FAMILY_LIQUIVISION_LYNX, 3, DC_TRANSPORT_SERIAL, NULL},
	/* Sporasub */
	{"Sporasub", "SP2", DC_FAMILY_SPORASUB_SP2, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Deep Six Excursion */
	{"Deep Six", "Excursion", DC_FAMILY_DEEPSIX_EXCURSION, 0, DC_TRANSPORT_BLE, dc_filter_deepsix},
	{"Crest",    "CR-4",      DC_FAMILY_DEEPSIX_EXCURSION, 0, DC_TRANSPORT_BLE, dc_filter_deepsix},
	{"Genesis",  "Centauri",  DC_FAMILY_DEEPSIX_EXCURSION, 0, DC_TRANSPORT_BLE, dc_filter_deepsix},
	{"Scorpena", "Alpha",     DC_FAMILY_DEEPSIX_EXCURSION, 0, DC_TRANSPORT_BLE, dc_filter_deepsix},
	/* Seac Screen */
	{"Seac", "Screen", DC_FAMILY_SEAC_SCREEN, 0, DC_TRANSPORT_SERIAL, NULL},
	{"Seac", "Action", DC_FAMILY_SEAC_SCREEN, 0, DC_TRANSPORT_SERIAL, NULL},
	/* Deepblu Cosmiq */
	{"Deepblu", "Cosmiq+", DC_FAMILY_DEEPBLU_COSMIQ, 0, DC_TRANSPORT_BLE, dc_filter_deepblu},
	/* Oceans S1 */
	{"Oceans", "S1", DC_FAMILY_OCEANS_S1, 0, DC_TRANSPORT_BLE, dc_filter_oceans},
	/* Divesoft Freedom */
	{"Divesoft", "Freedom", DC_FAMILY_DIVESOFT_FREEDOM, 19, DC_TRANSPORT_BLE, dc_filter_divesoft},
	{"Divesoft", "Liberty", DC_FAMILY_DIVESOFT_FREEDOM, 10, DC_TRANSPORT_BLE, dc_filter_divesoft},
};

static int
dc_match_name (const void *key, const void *value)
{
	const char *k = (const char *) key;
	const char *v = *(const char * const *) value;

	return strcasecmp (k, v) == 0;
}

static int
dc_match_prefix (const void *key, const void *value)
{
	const char *k = (const char *) key;
	const char *v = *(const char * const *) value;

	return strncasecmp (k, v, strlen (v)) == 0;
}

static int
dc_match_devname (const void *key, const void *value)
{
	const char *k = (const char *) key;
	const char *v = *(const char * const *) value;

	return strncmp (k, v, strlen (v)) == 0;
}

static int
dc_match_usb (const void *key, const void *value)
{
	const dc_usb_desc_t *k = (const dc_usb_desc_t *) key;
	const dc_usb_desc_t *v = (const dc_usb_desc_t *) value;

	return k->vid == v->vid && k->pid == v->pid;
}

static int
dc_match_usbhid (const void *key, const void *value)
{
	const dc_usbhid_desc_t *k = (const dc_usbhid_desc_t *) key;
	const dc_usbhid_desc_t *v = (const dc_usbhid_desc_t *) value;

	return k->vid == v->vid && k->pid == v->pid;
}

static int
dc_match_number_with_prefix (const void *key, const void *value)
{
	const char *str = (const char *) key;
	const char *prefix = *(const char * const *) value;

	size_t n = strlen (prefix);

	if (strncmp (str, prefix, n) != 0) {
		return 0;
	}

	while (str[n] != 0) {
		const char c = str[n];
		if (c < '0' || c > '9') {
			return 0;
		}
		n++;
	}

	return 1;
}

static int
dc_match_oceanic (const void *key, const void *value)
{
	unsigned int model = *(const unsigned int *) value;

	const char prefix[] = {
		(model >> 8) & 0xFF,
		(model     ) & 0xFF,
		0
	};

	const char *p = prefix;

	return dc_match_number_with_prefix (key, &p);
}

static int
dc_filter_internal (const void *key, const void *values, size_t count, size_t size, dc_match_t match)
{
	if (key == NULL)
		return 1;

	for (size_t i = 0; i < count; ++i) {
		if (match (key, (const unsigned char *) values + i * size)) {
			return 1;
		}
	}

	return count == 0;
}

static const char * const rfcomm[] = {
#if defined (__linux__)
	"/dev/rfcomm",
#endif
	NULL
};

static int
dc_filter_uwatec (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const irda[] = {
		"Aladin Smart Com",
		"Aladin Smart Pro",
		"Aladin Smart Tec",
		"Aladin Smart Z",
		"Uwatec Aladin",
		"UWATEC Galileo",
		"UWATEC Galileo Sol",
	};
	static const dc_usbhid_desc_t usbhid[] = {
		{0x2e6c, 0x3201}, // G2, G2 TEK
		{0x2e6c, 0x3211}, // G2 Console
		{0x2e6c, 0x4201}, // G2 HUD
		{0xc251, 0x2006}, // Aladin Square
	};
	static const char * const bluetooth[] = {
		"G2",
		"Aladin",
		"HUD",
		"A1",
		"A2",
		"G2 TEK",
		"Galileo 3",
		"Luna 2.0 AI",
		"Luna 2.0",
	};

	if (transport == DC_TRANSPORT_IRDA) {
		return DC_FILTER_INTERNAL (userdata, irda, 0, dc_match_name);
	} else if (transport == DC_TRANSPORT_USBHID) {
		return DC_FILTER_INTERNAL (userdata, usbhid, 0, dc_match_usbhid);
	} else if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	}

	return 1;
}

static int
dc_filter_suunto (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const dc_usbhid_desc_t usbhid[] = {
		{0x1493, 0x0030}, // Eon Steel
		{0x1493, 0x0033}, // Eon Core
		{0x1493, 0x0035}, // D5
		{0x1493, 0x0036}, // EON Steel Black
	};
	static const char * const bluetooth[] = {
		"EON Steel",
		"EON Core",
		"Suunto D5",
		"EON Steel Black",
	};

	if (transport == DC_TRANSPORT_USBHID) {
		return DC_FILTER_INTERNAL (userdata, usbhid, 0, dc_match_usbhid);
	} else if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_prefix);
	}

	return 1;
}

static int
dc_filter_hw (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"OSTC",
		"FROG",
	};

	if (transport == DC_TRANSPORT_BLUETOOTH || transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_prefix);
	} else if (transport == DC_TRANSPORT_SERIAL) {
		return DC_FILTER_INTERNAL (userdata, rfcomm, 1, dc_match_devname);
	}

	return 1;
}

static int
dc_filter_shearwater (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"Predator",
		"Petrel",
		"Petrel 3",
		"NERD",
		"NERD 2",
		"Perdix",
		"Perdix 2",
		"Teric",
		"Peregrine",
		"Tern"
	};

	if (transport == DC_TRANSPORT_BLUETOOTH || transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	} else if (transport == DC_TRANSPORT_SERIAL) {
		return DC_FILTER_INTERNAL (userdata, rfcomm, 1, dc_match_devname);
	}

	return 1;
}

static int
dc_filter_tecdiving (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"DiveComputer",
	};

	if (transport == DC_TRANSPORT_BLUETOOTH) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	} else if (transport == DC_TRANSPORT_SERIAL) {
		return DC_FILTER_INTERNAL (userdata, rfcomm, 1, dc_match_devname);
	}

	return 1;
}

static int
dc_filter_mares (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"Mares bluelink pro",
		"Mares Genius",
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_prefix);
	}

	return 1;
}

static int
dc_filter_divesystem (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"DS",
		"IX5M",
		"RATIO-",
	};

	if (transport == DC_TRANSPORT_BLUETOOTH || transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_number_with_prefix);
	}

	return 1;
}

static int
dc_filter_oceanic (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const unsigned int model[] = {
		0x4552, // Oceanic Pro Plus X
		0x455A, // Aqualung i750TC
		0x4647, // Sherwood Sage
		0x4648, // Aqualung i300C
		0x4649, // Aqualung i200C
		0x4651, // Aqualung i770R
		0x4652, // Aqualung i550C
		0x4653, // Oceanic Geo 4.0
		0x4654, // Oceanic Veo 4.0
		0x4655, // Sherwood Wisdom 4
		0x4656, // Oceanic Pro Plus 4
		0x4742, // Sherwood Beacon
		0x4743, // Aqualung i470TC
		0x4749, // Aqualung i200C
		0x474B, // Oceanic Geo Air
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, model, 0, dc_match_oceanic);
	}

	return 1;
}

static int
dc_filter_mclean(dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"McLean Extreme",
	};

	if (transport == DC_TRANSPORT_BLUETOOTH || transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	} else if (transport == DC_TRANSPORT_SERIAL) {
		return DC_FILTER_INTERNAL(userdata, rfcomm, 1, dc_match_devname);
	}

	return 1;
}

static int
dc_filter_atomic (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const dc_usb_desc_t usb[] = {
		{0x0471, 0x0888}, // Atomic Aquatics Cobalt
	};

	if (transport == DC_TRANSPORT_USB) {
		return DC_FILTER_INTERNAL (userdata, usb, 0, dc_match_usb);
	}

	return 1;
}

static int
dc_filter_deepsix (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"EXCURSION",
		"Crest-CR4",
		"CENTAURI",
		"ALPHA",
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	}

	return 1;
}

static int
dc_filter_deepblu (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"COSMIQ",
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_name);
	}

	return 1;
}

static int
dc_filter_oceans (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"S1",
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_prefix);
	}

	return 1;
}

static int
dc_filter_divesoft (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	static const char * const bluetooth[] = {
		"Freedom",
		"Liberty",
	};

	if (transport == DC_TRANSPORT_BLE) {
		return DC_FILTER_INTERNAL (userdata, bluetooth, 0, dc_match_prefix);
	}

	return 1;
}

dc_status_t
dc_descriptor_iterator (dc_iterator_t **out)
{
	dc_descriptor_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_descriptor_iterator_t *) dc_iterator_allocate (NULL, &dc_descriptor_iterator_vtable);
	if (iterator == NULL)
		return DC_STATUS_NOMEMORY;

	iterator->current = 0;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_descriptor_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_descriptor_iterator_t *iterator = (dc_descriptor_iterator_t *) abstract;
	dc_descriptor_t **item = (dc_descriptor_t **) out;

	if (iterator->current >= C_ARRAY_SIZE (g_descriptors))
		return DC_STATUS_DONE;

	/*
	 * The explicit cast from a const to a non-const pointer is safe here. The
	 * public interface doesn't support write access, and therefore descriptor
	 * objects are always read-only. However, the cast allows to return a direct
	 * reference to the entries in the table, avoiding the overhead of
	 * allocating (and freeing) memory for a deep copy.
	 */
	*item = (dc_descriptor_t *) &g_descriptors[iterator->current++];

	return DC_STATUS_SUCCESS;
}

void
dc_descriptor_free (dc_descriptor_t *descriptor)
{
	return;
}

const char *
dc_descriptor_get_vendor (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return NULL;

	return descriptor->vendor;
}

const char *
dc_descriptor_get_product (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return NULL;

	return descriptor->product;
}

dc_family_t
dc_descriptor_get_type (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return DC_FAMILY_NULL;

	return descriptor->type;
}

unsigned int
dc_descriptor_get_model (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return 0;

	return descriptor->model;
}

unsigned int
dc_descriptor_get_transports (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return DC_TRANSPORT_NONE;

	return descriptor->transports;
}

int
dc_descriptor_filter (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata)
{
	if (descriptor == NULL || descriptor->filter == NULL || userdata == NULL)
		return 1;

	return descriptor->filter (descriptor, transport, userdata);
}
