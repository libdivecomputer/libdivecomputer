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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_HIDAPI)
#define USBHID
#elif defined(HAVE_LIBUSB) && !defined(__APPLE__)
#define USBHID
#endif

#ifdef _WIN32
#ifdef HAVE_AF_IRDA_H
#define IRDA
#endif
#else
#ifdef HAVE_LINUX_IRDA_H
#define IRDA
#endif
#endif

#include <stddef.h>
#include <stdlib.h>

#include "descriptor-private.h"
#include "iterator-private.h"

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

static dc_status_t dc_descriptor_iterator_next (dc_iterator_t *iterator, void *item);

struct dc_descriptor_t {
	const char *vendor;
	const char *product;
	dc_family_t type;
	unsigned int model;
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
	{"Suunto", "Solution", DC_FAMILY_SUUNTO_SOLUTION, 0, NULL},
	/* Suunto Eon */
	{"Suunto", "Eon",             DC_FAMILY_SUUNTO_EON, 0, NULL},
	{"Suunto", "Solution Alpha",  DC_FAMILY_SUUNTO_EON, 0, NULL},
	{"Suunto", "Solution Nitrox", DC_FAMILY_SUUNTO_EON, 0, NULL},
	/* Suunto Vyper */
	{"Suunto", "Spyder",   DC_FAMILY_SUUNTO_VYPER, 0x01, NULL},
	{"Suunto", "Stinger",  DC_FAMILY_SUUNTO_VYPER, 0x03, NULL},
	{"Suunto", "Mosquito", DC_FAMILY_SUUNTO_VYPER, 0x04, NULL},
	{"Suunto", "D3",       DC_FAMILY_SUUNTO_VYPER, 0x05, NULL},
	{"Suunto", "Vyper",    DC_FAMILY_SUUNTO_VYPER, 0x0A, NULL},
	{"Suunto", "Vytec",    DC_FAMILY_SUUNTO_VYPER, 0X0B, NULL},
	{"Suunto", "Cobra",    DC_FAMILY_SUUNTO_VYPER, 0X0C, NULL},
	{"Suunto", "Gekko",    DC_FAMILY_SUUNTO_VYPER, 0X0D, NULL},
	{"Suunto", "Zoop",     DC_FAMILY_SUUNTO_VYPER, 0x16, NULL},
	/* Suunto Vyper 2 */
	{"Suunto", "Vyper 2",   DC_FAMILY_SUUNTO_VYPER2, 0x10, NULL},
	{"Suunto", "Cobra 2",   DC_FAMILY_SUUNTO_VYPER2, 0x11, NULL},
	{"Suunto", "Vyper Air", DC_FAMILY_SUUNTO_VYPER2, 0x13, NULL},
	{"Suunto", "Cobra 3",   DC_FAMILY_SUUNTO_VYPER2, 0x14, NULL},
	{"Suunto", "HelO2",     DC_FAMILY_SUUNTO_VYPER2, 0x15, NULL},
	/* Suunto D9 */
	{"Suunto", "D9",         DC_FAMILY_SUUNTO_D9, 0x0E, NULL},
	{"Suunto", "D6",         DC_FAMILY_SUUNTO_D9, 0x0F, NULL},
	{"Suunto", "D4",         DC_FAMILY_SUUNTO_D9, 0x12, NULL},
	{"Suunto", "D4i",        DC_FAMILY_SUUNTO_D9, 0x19, NULL},
	{"Suunto", "D6i",        DC_FAMILY_SUUNTO_D9, 0x1A, NULL},
	{"Suunto", "D9tx",       DC_FAMILY_SUUNTO_D9, 0x1B, NULL},
	{"Suunto", "DX",         DC_FAMILY_SUUNTO_D9, 0x1C, NULL},
	{"Suunto", "Vyper Novo", DC_FAMILY_SUUNTO_D9, 0x1D, NULL},
	{"Suunto", "Zoop Novo",  DC_FAMILY_SUUNTO_D9, 0x1E, NULL},
	{"Suunto", "D4f",        DC_FAMILY_SUUNTO_D9, 0x20, NULL},
	/* Suunto EON Steel */
#ifdef USBHID
	{"Suunto", "EON Steel", DC_FAMILY_SUUNTO_EONSTEEL, 0, NULL},
	{"Suunto", "EON Core",  DC_FAMILY_SUUNTO_EONSTEEL, 1, NULL},
#endif
	/* Uwatec Aladin */
	{"Uwatec", "Aladin Air Twin",     DC_FAMILY_UWATEC_ALADIN, 0x1C, NULL},
	{"Uwatec", "Aladin Sport Plus",   DC_FAMILY_UWATEC_ALADIN, 0x3E, NULL},
	{"Uwatec", "Aladin Pro",          DC_FAMILY_UWATEC_ALADIN, 0x3F, NULL},
	{"Uwatec", "Aladin Air Z",        DC_FAMILY_UWATEC_ALADIN, 0x44, NULL},
	{"Uwatec", "Aladin Air Z O2",     DC_FAMILY_UWATEC_ALADIN, 0xA4, NULL},
	{"Uwatec", "Aladin Air Z Nitrox", DC_FAMILY_UWATEC_ALADIN, 0xF4, NULL},
	{"Uwatec", "Aladin Pro Ultra",    DC_FAMILY_UWATEC_ALADIN, 0xFF, NULL},
	/* Uwatec Memomouse */
	{"Uwatec", "Memomouse", DC_FAMILY_UWATEC_MEMOMOUSE, 0, NULL},
	/* Uwatec Smart */
#ifdef IRDA
	{"Uwatec", "Smart Pro",     DC_FAMILY_UWATEC_SMART, 0x10, NULL},
	{"Uwatec", "Galileo Sol",   DC_FAMILY_UWATEC_SMART, 0x11, NULL},
	{"Uwatec", "Galileo Luna",  DC_FAMILY_UWATEC_SMART, 0x11, NULL},
	{"Uwatec", "Galileo Terra", DC_FAMILY_UWATEC_SMART, 0x11, NULL},
	{"Uwatec", "Aladin Tec",    DC_FAMILY_UWATEC_SMART, 0x12, NULL},
	{"Uwatec", "Aladin Prime",  DC_FAMILY_UWATEC_SMART, 0x12, NULL},
	{"Uwatec", "Aladin Tec 2G", DC_FAMILY_UWATEC_SMART, 0x13, NULL},
	{"Uwatec", "Aladin 2G",     DC_FAMILY_UWATEC_SMART, 0x13, NULL},
	{"Subgear","XP-10",         DC_FAMILY_UWATEC_SMART, 0x13, NULL},
	{"Uwatec", "Smart Com",     DC_FAMILY_UWATEC_SMART, 0x14, NULL},
	{"Uwatec", "Aladin 2G",     DC_FAMILY_UWATEC_SMART, 0x15, NULL},
	{"Uwatec", "Aladin Tec 3G", DC_FAMILY_UWATEC_SMART, 0x15, NULL},
	{"Uwatec", "Aladin Sport",  DC_FAMILY_UWATEC_SMART, 0x15, NULL},
	{"Subgear","XP-3G",         DC_FAMILY_UWATEC_SMART, 0x15, NULL},
	{"Uwatec", "Smart Tec",     DC_FAMILY_UWATEC_SMART, 0x18, NULL},
	{"Uwatec", "Galileo Trimix",DC_FAMILY_UWATEC_SMART, 0x19, NULL},
	{"Uwatec", "Smart Z",       DC_FAMILY_UWATEC_SMART, 0x1C, NULL},
	{"Subgear","XP Air",        DC_FAMILY_UWATEC_SMART, 0x1C, NULL},
#endif
	/* Scubapro/Uwatec Meridian */
	{"Scubapro", "Meridian",    DC_FAMILY_UWATEC_MERIDIAN, 0x20, NULL},
	{"Scubapro", "Mantis",      DC_FAMILY_UWATEC_MERIDIAN, 0x20, NULL},
	{"Scubapro", "Chromis",     DC_FAMILY_UWATEC_MERIDIAN, 0x24, NULL},
	{"Scubapro", "Mantis 2",    DC_FAMILY_UWATEC_MERIDIAN, 0x26, NULL},
	/* Scubapro G2 */
#ifdef USBHID
	{"Scubapro", "Aladin Sport Matrix", DC_FAMILY_UWATEC_G2, 0x17, NULL},
	{"Scubapro", "Aladin Square",       DC_FAMILY_UWATEC_G2, 0x22, NULL},
	{"Scubapro", "G2",                  DC_FAMILY_UWATEC_G2, 0x32, NULL},
#endif
	/* Reefnet */
	{"Reefnet", "Sensus",       DC_FAMILY_REEFNET_SENSUS, 1, NULL},
	{"Reefnet", "Sensus Pro",   DC_FAMILY_REEFNET_SENSUSPRO, 2, NULL},
	{"Reefnet", "Sensus Ultra", DC_FAMILY_REEFNET_SENSUSULTRA, 3, NULL},
	/* Oceanic VT Pro */
	{"Aeris",    "500 AI",     DC_FAMILY_OCEANIC_VTPRO, 0x4151, NULL},
	{"Oceanic",  "Versa Pro",  DC_FAMILY_OCEANIC_VTPRO, 0x4155, NULL},
	{"Aeris",    "Atmos 2",    DC_FAMILY_OCEANIC_VTPRO, 0x4158, NULL},
	{"Oceanic",  "Pro Plus 2", DC_FAMILY_OCEANIC_VTPRO, 0x4159, NULL},
	{"Aeris",    "Atmos AI",   DC_FAMILY_OCEANIC_VTPRO, 0x4244, NULL},
	{"Oceanic",  "VT Pro",     DC_FAMILY_OCEANIC_VTPRO, 0x4245, NULL},
	{"Sherwood", "Wisdom",     DC_FAMILY_OCEANIC_VTPRO, 0x4246, NULL},
	{"Aeris",    "Elite",      DC_FAMILY_OCEANIC_VTPRO, 0x424F, NULL},
	/* Oceanic Veo 250 */
	{"Genesis", "React Pro", DC_FAMILY_OCEANIC_VEO250, 0x4247, NULL},
	{"Oceanic", "Veo 200",   DC_FAMILY_OCEANIC_VEO250, 0x424B, NULL},
	{"Oceanic", "Veo 250",   DC_FAMILY_OCEANIC_VEO250, 0x424C, NULL},
	{"Seemann", "XP5",       DC_FAMILY_OCEANIC_VEO250, 0x4251, NULL},
	{"Oceanic", "Veo 180",   DC_FAMILY_OCEANIC_VEO250, 0x4252, NULL},
	{"Aeris",   "XR-2",      DC_FAMILY_OCEANIC_VEO250, 0x4255, NULL},
	{"Sherwood", "Insight",  DC_FAMILY_OCEANIC_VEO250, 0x425A, NULL},
	{"Hollis",  "DG02",      DC_FAMILY_OCEANIC_VEO250, 0x4352, NULL},
	/* Oceanic Atom 2.0 */
	{"Oceanic",  "Atom 1.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4250, NULL},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4257, NULL},
	{"Oceanic",  "VT3",                 DC_FAMILY_OCEANIC_ATOM2, 0x4258, NULL},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4259, NULL},
	{"Oceanic",  "Atom 2.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4342, NULL},
	{"Oceanic",  "Geo",                 DC_FAMILY_OCEANIC_ATOM2, 0x4344, NULL},
	{"Aeris",    "Manta",               DC_FAMILY_OCEANIC_ATOM2, 0x4345, NULL},
	{"Aeris",    "XR-1 NX",             DC_FAMILY_OCEANIC_ATOM2, 0x4346, NULL},
	{"Oceanic",  "Datamask",            DC_FAMILY_OCEANIC_ATOM2, 0x4347, NULL},
	{"Aeris",    "Compumask",           DC_FAMILY_OCEANIC_ATOM2, 0x4348, NULL},
	{"Aeris",    "F10",                 DC_FAMILY_OCEANIC_ATOM2, 0x434D, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x434E, NULL},
	{"Sherwood", "Wisdom 2",            DC_FAMILY_OCEANIC_ATOM2, 0x4350, NULL},
	{"Sherwood", "Insight 2",           DC_FAMILY_OCEANIC_ATOM2, 0x4353, NULL},
	{"Genesis",  "React Pro White",     DC_FAMILY_OCEANIC_ATOM2, 0x4354, NULL},
	{"Tusa",     "Element II (IQ-750)", DC_FAMILY_OCEANIC_ATOM2, 0x4357, NULL},
	{"Oceanic",  "Veo 1.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4358, NULL},
	{"Oceanic",  "Veo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4359, NULL},
	{"Oceanic",  "Veo 3.0",             DC_FAMILY_OCEANIC_ATOM2, 0x435A, NULL},
	{"Tusa",     "Zen (IQ-900)",        DC_FAMILY_OCEANIC_ATOM2, 0x4441, NULL},
	{"Tusa",     "Zen Air (IQ-950)",    DC_FAMILY_OCEANIC_ATOM2, 0x4442, NULL},
	{"Aeris",    "Atmos AI 2",          DC_FAMILY_OCEANIC_ATOM2, 0x4443, NULL},
	{"Oceanic",  "Pro Plus 2.1",        DC_FAMILY_OCEANIC_ATOM2, 0x4444, NULL},
	{"Oceanic",  "Geo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4446, NULL},
	{"Oceanic",  "VT4",                 DC_FAMILY_OCEANIC_ATOM2, 0x4447, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4449, NULL},
	{"Beuchat",  "Voyager 2G",          DC_FAMILY_OCEANIC_ATOM2, 0x444B, NULL},
	{"Oceanic",  "Atom 3.0",            DC_FAMILY_OCEANIC_ATOM2, 0x444C, NULL},
	{"Hollis",   "DG03",                DC_FAMILY_OCEANIC_ATOM2, 0x444D, NULL},
	{"Oceanic",  "OCS",                 DC_FAMILY_OCEANIC_ATOM2, 0x4450, NULL},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4451, NULL},
	{"Oceanic",  "VT 4.1",              DC_FAMILY_OCEANIC_ATOM2, 0x4452, NULL},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4453, NULL},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4455, NULL},
	{"Oceanic",  "Atom 3.1",            DC_FAMILY_OCEANIC_ATOM2, 0x4456, NULL},
	{"Aeris",    "A300 AI",             DC_FAMILY_OCEANIC_ATOM2, 0x4457, NULL},
	{"Sherwood", "Wisdom 3",            DC_FAMILY_OCEANIC_ATOM2, 0x4458, NULL},
	{"Aeris",    "A300",                DC_FAMILY_OCEANIC_ATOM2, 0x445A, NULL},
	{"Hollis",   "TX1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4542, NULL},
	{"Beuchat",  "Mundial 2",           DC_FAMILY_OCEANIC_ATOM2, 0x4543, NULL},
	{"Sherwood", "Amphos",              DC_FAMILY_OCEANIC_ATOM2, 0x4545, NULL},
	{"Sherwood", "Amphos Air",          DC_FAMILY_OCEANIC_ATOM2, 0x4546, NULL},
	{"Oceanic",  "Pro Plus 3",          DC_FAMILY_OCEANIC_ATOM2, 0x4548, NULL},
	{"Aeris",    "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4549, NULL},
	{"Oceanic",  "OCi",                 DC_FAMILY_OCEANIC_ATOM2, 0x454B, NULL},
	{"Aeris",    "A300CS",              DC_FAMILY_OCEANIC_ATOM2, 0x454C, NULL},
	{"Beuchat",  "Mundial 3",           DC_FAMILY_OCEANIC_ATOM2, 0x4550, NULL},
	{"Oceanic",  "F10",                 DC_FAMILY_OCEANIC_ATOM2, 0x4553, NULL},
	{"Oceanic",  "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4554, NULL},
	{"Subgear",  "XP-Air",              DC_FAMILY_OCEANIC_ATOM2, 0x4555, NULL},
	{"Sherwood", "Vision",              DC_FAMILY_OCEANIC_ATOM2, 0x4556, NULL},
	{"Oceanic",  "VTX",                 DC_FAMILY_OCEANIC_ATOM2, 0x4557, NULL},
	{"Aqualung", "i300",                DC_FAMILY_OCEANIC_ATOM2, 0x4559, NULL},
	{"Aqualung", "i750TC",              DC_FAMILY_OCEANIC_ATOM2, 0x455A, NULL},
	{"Aqualung", "i450T",               DC_FAMILY_OCEANIC_ATOM2, 0x4641, NULL},
	{"Aqualung", "i550",                DC_FAMILY_OCEANIC_ATOM2, 0x4642, NULL},
	{"Aqualung", "i200",                DC_FAMILY_OCEANIC_ATOM2, 0x4646, NULL},
	/* Mares Nemo */
	{"Mares", "Nemo",         DC_FAMILY_MARES_NEMO, 0, NULL},
	{"Mares", "Nemo Steel",   DC_FAMILY_MARES_NEMO, 0, NULL},
	{"Mares", "Nemo Titanium",DC_FAMILY_MARES_NEMO, 0, NULL},
	{"Mares", "Nemo Excel",   DC_FAMILY_MARES_NEMO, 17, NULL},
	{"Mares", "Nemo Apneist", DC_FAMILY_MARES_NEMO, 18, NULL},
	/* Mares Puck */
	{"Mares", "Puck",      DC_FAMILY_MARES_PUCK, 7, NULL},
	{"Mares", "Puck Air",  DC_FAMILY_MARES_PUCK, 19, NULL},
	{"Mares", "Nemo Air",  DC_FAMILY_MARES_PUCK, 4, NULL},
	{"Mares", "Nemo Wide", DC_FAMILY_MARES_PUCK, 1, NULL},
	/* Mares Darwin */
	{"Mares", "Darwin",     DC_FAMILY_MARES_DARWIN , 0, NULL},
	{"Mares", "M1",         DC_FAMILY_MARES_DARWIN , 0, NULL},
	{"Mares", "M2",         DC_FAMILY_MARES_DARWIN , 0, NULL},
	{"Mares", "Darwin Air", DC_FAMILY_MARES_DARWIN , 1, NULL},
	{"Mares", "Airlab",     DC_FAMILY_MARES_DARWIN , 1, NULL},
	/* Mares Icon HD */
	{"Mares", "Matrix",            DC_FAMILY_MARES_ICONHD , 0x0F, NULL},
	{"Mares", "Smart",             DC_FAMILY_MARES_ICONHD , 0x000010, NULL},
	{"Mares", "Smart Apnea",       DC_FAMILY_MARES_ICONHD , 0x010010, NULL},
	{"Mares", "Icon HD",           DC_FAMILY_MARES_ICONHD , 0x14, NULL},
	{"Mares", "Icon HD Net Ready", DC_FAMILY_MARES_ICONHD , 0x15, NULL},
	{"Mares", "Puck Pro",          DC_FAMILY_MARES_ICONHD , 0x18, NULL},
	{"Mares", "Nemo Wide 2",       DC_FAMILY_MARES_ICONHD , 0x19, NULL},
	{"Mares", "Puck 2",            DC_FAMILY_MARES_ICONHD , 0x1F, NULL},
	{"Mares", "Quad Air",          DC_FAMILY_MARES_ICONHD , 0x23, NULL},
	{"Mares", "Quad",              DC_FAMILY_MARES_ICONHD , 0x29, NULL},
	/* Heinrichs Weikamp */
	{"Heinrichs Weikamp", "OSTC",     DC_FAMILY_HW_OSTC, 0, NULL},
	{"Heinrichs Weikamp", "OSTC Mk2", DC_FAMILY_HW_OSTC, 1, NULL},
	{"Heinrichs Weikamp", "OSTC 2N",  DC_FAMILY_HW_OSTC, 2, NULL},
	{"Heinrichs Weikamp", "OSTC 2C",  DC_FAMILY_HW_OSTC, 3, NULL},
	{"Heinrichs Weikamp", "Frog",     DC_FAMILY_HW_FROG, 0, NULL},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x11, NULL},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x13, NULL},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x1B, NULL},
	{"Heinrichs Weikamp", "OSTC 3",     DC_FAMILY_HW_OSTC3, 0x0A, NULL},
	{"Heinrichs Weikamp", "OSTC Plus",  DC_FAMILY_HW_OSTC3, 0x13, NULL},
	{"Heinrichs Weikamp", "OSTC Plus",  DC_FAMILY_HW_OSTC3, 0x1A, NULL},
	{"Heinrichs Weikamp", "OSTC 4",     DC_FAMILY_HW_OSTC3, 0x3B, NULL},
	{"Heinrichs Weikamp", "OSTC cR",    DC_FAMILY_HW_OSTC3, 0x05, NULL},
	{"Heinrichs Weikamp", "OSTC cR",    DC_FAMILY_HW_OSTC3, 0x07, NULL},
	{"Heinrichs Weikamp", "OSTC Sport", DC_FAMILY_HW_OSTC3, 0x12, NULL},
	{"Heinrichs Weikamp", "OSTC Sport", DC_FAMILY_HW_OSTC3, 0x13, NULL},
	/* Cressi Edy */
	{"Tusa",   "IQ-700", DC_FAMILY_CRESSI_EDY, 0x05, NULL},
	{"Cressi", "Edy",    DC_FAMILY_CRESSI_EDY, 0x08, NULL},
	/* Cressi Leonardo */
	{"Cressi", "Leonardo", DC_FAMILY_CRESSI_LEONARDO, 1, NULL},
	{"Cressi", "Giotto",   DC_FAMILY_CRESSI_LEONARDO, 4, NULL},
	{"Cressi", "Newton",   DC_FAMILY_CRESSI_LEONARDO, 5, NULL},
	{"Cressi", "Drake",    DC_FAMILY_CRESSI_LEONARDO, 6, NULL},
	/* Zeagle N2iTiON3 */
	{"Zeagle",    "N2iTiON3",   DC_FAMILY_ZEAGLE_N2ITION3, 0, NULL},
	{"Apeks",     "Quantum X",  DC_FAMILY_ZEAGLE_N2ITION3, 0, NULL},
	{"Dive Rite", "NiTek Trio", DC_FAMILY_ZEAGLE_N2ITION3, 0, NULL},
	{"Scubapro",  "XTender 5",  DC_FAMILY_ZEAGLE_N2ITION3, 0, NULL},
	/* Atomic Aquatics Cobalt */
#ifdef HAVE_LIBUSB
	{"Atomic Aquatics", "Cobalt", DC_FAMILY_ATOMICS_COBALT, 0, NULL},
	{"Atomic Aquatics", "Cobalt 2", DC_FAMILY_ATOMICS_COBALT, 2, NULL},
#endif
	/* Shearwater Predator */
	{"Shearwater", "Predator", DC_FAMILY_SHEARWATER_PREDATOR, 2, NULL},
	/* Shearwater Petrel */
	{"Shearwater", "Petrel",    DC_FAMILY_SHEARWATER_PETREL, 3, NULL},
	{"Shearwater", "Petrel 2",  DC_FAMILY_SHEARWATER_PETREL, 3, NULL},
	{"Shearwater", "Nerd",      DC_FAMILY_SHEARWATER_PETREL, 4, NULL},
	{"Shearwater", "Perdix",    DC_FAMILY_SHEARWATER_PETREL, 5, NULL},
	{"Shearwater", "Perdix AI", DC_FAMILY_SHEARWATER_PETREL, 6, NULL},
	{"Shearwater", "Nerd 2",    DC_FAMILY_SHEARWATER_PETREL, 7, NULL},
	/* Dive Rite NiTek Q */
	{"Dive Rite", "NiTek Q",   DC_FAMILY_DIVERITE_NITEKQ, 0, NULL},
	/* Citizen Hyper Aqualand */
	{"Citizen", "Hyper Aqualand", DC_FAMILY_CITIZEN_AQUALAND, 0, NULL},
	/* DiveSystem/Ratio iDive */
	{"DiveSystem", "Orca",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x02, NULL},
	{"DiveSystem", "iDive Pro",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x03, NULL},
	{"DiveSystem", "iDive DAN",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x04, NULL},
	{"DiveSystem", "iDive Tech",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x05, NULL},
	{"DiveSystem", "iDive Reb",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x06, NULL},
	{"DiveSystem", "iDive Stealth", DC_FAMILY_DIVESYSTEM_IDIVE, 0x07, NULL},
	{"DiveSystem", "iDive Free",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x08, NULL},
	{"DiveSystem", "iDive Easy",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x09, NULL},
	{"DiveSystem", "iDive X3M",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x0A, NULL},
	{"DiveSystem", "iDive Deep",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x0B, NULL},
	{"Ratio",      "iX3M Easy",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x22, NULL},
	{"Ratio",      "iX3M Deep",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x23, NULL},
	{"Ratio",      "iX3M Tech+",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x24, NULL},
	{"Ratio",      "iX3M Reb",      DC_FAMILY_DIVESYSTEM_IDIVE, 0x25, NULL},
	{"Ratio",      "iX3M Pro Easy", DC_FAMILY_DIVESYSTEM_IDIVE, 0x32, NULL},
	{"Ratio",      "iX3M Pro Deep", DC_FAMILY_DIVESYSTEM_IDIVE, 0x34, NULL},
	{"Ratio",      "iX3M Pro Tech+",DC_FAMILY_DIVESYSTEM_IDIVE, 0x35, NULL},
	{"Ratio",      "iDive Free",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x40, NULL},
	{"Ratio",      "iDive Easy",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x42, NULL},
	{"Ratio",      "iDive Deep",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x44, NULL},
	{"Ratio",      "iDive Tech+",   DC_FAMILY_DIVESYSTEM_IDIVE, 0x45, NULL},
	{"Seac",       "Jack",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x1000, NULL},
	/* Cochran Commander */
	{"Cochran", "Commander TM", DC_FAMILY_COCHRAN_COMMANDER, 0, NULL},
	{"Cochran", "Commander I",  DC_FAMILY_COCHRAN_COMMANDER, 1, NULL},
	{"Cochran", "Commander II", DC_FAMILY_COCHRAN_COMMANDER, 2, NULL},
	{"Cochran", "EMC-14",       DC_FAMILY_COCHRAN_COMMANDER, 3, NULL},
	{"Cochran", "EMC-16",       DC_FAMILY_COCHRAN_COMMANDER, 4, NULL},
	{"Cochran", "EMC-20H",      DC_FAMILY_COCHRAN_COMMANDER, 5, NULL},
};

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

dc_transport_t
dc_descriptor_get_transport (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return DC_TRANSPORT_NONE;

	if (descriptor->type == DC_FAMILY_ATOMICS_COBALT)
		return DC_TRANSPORT_USB;
	else if (descriptor->type == DC_FAMILY_SUUNTO_EONSTEEL)
		return DC_TRANSPORT_USBHID;
	else if (descriptor->type == DC_FAMILY_UWATEC_G2)
		return DC_TRANSPORT_USBHID;
	else if (descriptor->type == DC_FAMILY_UWATEC_SMART)
		return DC_TRANSPORT_IRDA;
	else
		return DC_TRANSPORT_SERIAL;
}

dc_filter_t
dc_descriptor_get_filter (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return NULL;

	return descriptor->filter;
}
