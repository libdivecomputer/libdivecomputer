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

#include <stddef.h>
#include <stdlib.h>

#include <libdivecomputer/descriptor.h>

#include "iterator-private.h"

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

struct dc_descriptor_t {
	const char *vendor;
	const char *product;
	dc_family_t type;
	unsigned int model;
};

/*
 * The model numbers in the table are the actual model numbers reported by the
 * device. For devices where there is no model number available (or known), an
 * artifical number (starting at zero) is assigned.  If the model number isn't
 * actually used to identify individual models, identical values are assigned.
 */

static const dc_descriptor_t g_descriptors[] = {
	/* Suunto Solution */
	{"Suunto", "Solution", DC_FAMILY_SUUNTO_SOLUTION, 0},
	/* Suunto Eon */
	{"Suunto", "Eon",             DC_FAMILY_SUUNTO_EON, 0},
	{"Suunto", "Solution Alpha",  DC_FAMILY_SUUNTO_EON, 0},
	{"Suunto", "Solution Nitrox", DC_FAMILY_SUUNTO_EON, 0},
	/* Suunto Vyper */
	{"Suunto", "Spyder",   DC_FAMILY_SUUNTO_VYPER, 0x01},
	{"Suunto", "Stinger",  DC_FAMILY_SUUNTO_VYPER, 0x03},
	{"Suunto", "Mosquito", DC_FAMILY_SUUNTO_VYPER, 0x04},
	{"Suunto", "D3",       DC_FAMILY_SUUNTO_VYPER, 0x05},
	{"Suunto", "Vyper",    DC_FAMILY_SUUNTO_VYPER, 0x0A},
	{"Suunto", "Vytec",    DC_FAMILY_SUUNTO_VYPER, 0X0B},
	{"Suunto", "Cobra",    DC_FAMILY_SUUNTO_VYPER, 0X0C},
	{"Suunto", "Gekko",    DC_FAMILY_SUUNTO_VYPER, 0X0D},
	{"Suunto", "Zoop",     DC_FAMILY_SUUNTO_VYPER, 0x16},
	/* Suunto Vyper 2 */
	{"Suunto", "Vyper 2",   DC_FAMILY_SUUNTO_VYPER2, 0x10},
	{"Suunto", "Cobra 2",   DC_FAMILY_SUUNTO_VYPER2, 0x11},
	{"Suunto", "Vyper Air", DC_FAMILY_SUUNTO_VYPER2, 0x13},
	{"Suunto", "Cobra 3",   DC_FAMILY_SUUNTO_VYPER2, 0x14},
	{"Suunto", "HelO2",     DC_FAMILY_SUUNTO_VYPER2, 0x15},
	/* Suunto D9 */
	{"Suunto", "D9",   DC_FAMILY_SUUNTO_D9, 0x0E},
	{"Suunto", "D6",   DC_FAMILY_SUUNTO_D9, 0x0F},
	{"Suunto", "D4",   DC_FAMILY_SUUNTO_D9, 0x12},
	{"Suunto", "D4i",  DC_FAMILY_SUUNTO_D9, 0x19},
	{"Suunto", "D6i",  DC_FAMILY_SUUNTO_D9, 0x1A},
	{"Suunto", "D9tx", DC_FAMILY_SUUNTO_D9, 0x1B},
	{"Suunto", "DX",   DC_FAMILY_SUUNTO_D9, 0x1C},
	/* Suunto EON Steel */
#ifdef HAVE_LIBUSB
	{"Suunto", "EON Steel", DC_FAMILY_SUUNTO_EONSTEEL, 0},
#endif
	/* Uwatec Aladin */
	{"Uwatec", "Aladin Air Twin",     DC_FAMILY_UWATEC_ALADIN, 0x1C},
	{"Uwatec", "Aladin Sport Plus",   DC_FAMILY_UWATEC_ALADIN, 0x3E},
	{"Uwatec", "Aladin Pro",          DC_FAMILY_UWATEC_ALADIN, 0x3F},
	{"Uwatec", "Aladin Air Z",        DC_FAMILY_UWATEC_ALADIN, 0x44},
	{"Uwatec", "Aladin Air Z O2",     DC_FAMILY_UWATEC_ALADIN, 0xA4},
	{"Uwatec", "Aladin Air Z Nitrox", DC_FAMILY_UWATEC_ALADIN, 0xF4},
	{"Uwatec", "Aladin Pro Ultra",    DC_FAMILY_UWATEC_ALADIN, 0xFF},
	/* Uwatec Memomouse */
	{"Uwatec", "Memomouse", DC_FAMILY_UWATEC_MEMOMOUSE, 0},
	/* Uwatec Smart */
#ifdef HAVE_IRDA
	{"Uwatec", "Smart Pro",     DC_FAMILY_UWATEC_SMART, 0x10},
	{"Uwatec", "Galileo Sol",   DC_FAMILY_UWATEC_SMART, 0x11},
	{"Uwatec", "Galileo Luna",  DC_FAMILY_UWATEC_SMART, 0x11},
	{"Uwatec", "Galileo Terra", DC_FAMILY_UWATEC_SMART, 0x11},
	{"Uwatec", "Aladin Tec",    DC_FAMILY_UWATEC_SMART, 0x12},
	{"Uwatec", "Aladin Prime",  DC_FAMILY_UWATEC_SMART, 0x12},
	{"Uwatec", "Aladin Tec 2G", DC_FAMILY_UWATEC_SMART, 0x13},
	{"Uwatec", "Aladin 2G",     DC_FAMILY_UWATEC_SMART, 0x13},
	{"Subgear","XP-10",         DC_FAMILY_UWATEC_SMART, 0x13},
	{"Uwatec", "Smart Com",     DC_FAMILY_UWATEC_SMART, 0x14},
	{"Uwatec", "Aladin 2G",     DC_FAMILY_UWATEC_SMART, 0x15},
	{"Subgear","XP-3G",         DC_FAMILY_UWATEC_SMART, 0x15},
	{"Uwatec", "Smart Tec",     DC_FAMILY_UWATEC_SMART, 0x18},
	{"Uwatec", "Galileo Trimix",DC_FAMILY_UWATEC_SMART, 0x19},
	{"Uwatec", "Smart Z",       DC_FAMILY_UWATEC_SMART, 0x1C},
	{"Subgear","XP Air",        DC_FAMILY_UWATEC_SMART, 0x1C},
#endif
	/* Scubapro/Uwatec Meridian */
	{"Scubapro", "Meridian",    DC_FAMILY_UWATEC_MERIDIAN, 0x20},
	{"Scubapro", "Chromis",     DC_FAMILY_UWATEC_MERIDIAN, 0x24},
	/* Reefnet */
	{"Reefnet", "Sensus",       DC_FAMILY_REEFNET_SENSUS, 1},
	{"Reefnet", "Sensus Pro",   DC_FAMILY_REEFNET_SENSUSPRO, 2},
	{"Reefnet", "Sensus Ultra", DC_FAMILY_REEFNET_SENSUSULTRA, 3},
	/* Oceanic VT Pro */
	{"Oceanic",  "Versa Pro",  DC_FAMILY_OCEANIC_VTPRO, 0x4155},
	{"Aeris",    "Atmos 2",    DC_FAMILY_OCEANIC_VTPRO, 0x4158},
	{"Oceanic",  "Pro Plus 2", DC_FAMILY_OCEANIC_VTPRO, 0x4159},
	{"Aeris",    "Atmos AI",   DC_FAMILY_OCEANIC_VTPRO, 0x4244},
	{"Oceanic",  "VT Pro",     DC_FAMILY_OCEANIC_VTPRO, 0x4245},
	{"Sherwood", "Wisdom",     DC_FAMILY_OCEANIC_VTPRO, 0x4246},
	{"Aeris",    "Elite",      DC_FAMILY_OCEANIC_VTPRO, 0x424F},
	/* Oceanic Veo 250 */
	{"Genesis", "React Pro", DC_FAMILY_OCEANIC_VEO250, 0x4247},
	{"Oceanic", "Veo 200",   DC_FAMILY_OCEANIC_VEO250, 0x424B},
	{"Oceanic", "Veo 250",   DC_FAMILY_OCEANIC_VEO250, 0x424C},
	{"Seemann", "XP5",       DC_FAMILY_OCEANIC_VEO250, 0x4251},
	{"Oceanic", "Veo 180",   DC_FAMILY_OCEANIC_VEO250, 0x4252},
	{"Aeris",   "XR-2",      DC_FAMILY_OCEANIC_VEO250, 0x4255},
	{"Sherwood", "Insight",  DC_FAMILY_OCEANIC_VEO250, 0x425A},
	/* Oceanic Atom 2.0 */
	{"Oceanic",  "Atom 1.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4250},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4257},
	{"Oceanic",  "VT3",                 DC_FAMILY_OCEANIC_ATOM2, 0x4258},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4259},
	{"Oceanic",  "Atom 2.0",            DC_FAMILY_OCEANIC_ATOM2, 0x4342},
	{"Oceanic",  "Geo",                 DC_FAMILY_OCEANIC_ATOM2, 0x4344},
	{"Aeris",    "Manta",               DC_FAMILY_OCEANIC_ATOM2, 0x4345},
	{"Aeris",    "XR-1 NX",             DC_FAMILY_OCEANIC_ATOM2, 0x4346},
	{"Oceanic",  "Datamask",            DC_FAMILY_OCEANIC_ATOM2, 0x4347},
	{"Aeris",    "Compumask",           DC_FAMILY_OCEANIC_ATOM2, 0x4348},
	{"Aeris",    "F10",                 DC_FAMILY_OCEANIC_ATOM2, 0x434D},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x434E},
	{"Sherwood", "Wisdom 2",            DC_FAMILY_OCEANIC_ATOM2, 0x4350},
	{"Sherwood", "Insight 2",           DC_FAMILY_OCEANIC_ATOM2, 0x4353},
	{"Genesis",  "React Pro White",     DC_FAMILY_OCEANIC_ATOM2, 0x4354},
	{"Tusa",     "Element II (IQ-750)", DC_FAMILY_OCEANIC_ATOM2, 0x4357},
	{"Oceanic",  "Veo 1.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4358},
	{"Oceanic",  "Veo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4359},
	{"Oceanic",  "Veo 3.0",             DC_FAMILY_OCEANIC_ATOM2, 0x435A},
	{"Tusa",     "Zen (IQ-900)",        DC_FAMILY_OCEANIC_ATOM2, 0x4441},
	{"Tusa",     "Zen Air (IQ-950)",    DC_FAMILY_OCEANIC_ATOM2, 0x4442},
	{"Aeris",    "Atmos AI 2",          DC_FAMILY_OCEANIC_ATOM2, 0x4443},
	{"Oceanic",  "Pro Plus 2.1",        DC_FAMILY_OCEANIC_ATOM2, 0x4444},
	{"Oceanic",  "Geo 2.0",             DC_FAMILY_OCEANIC_ATOM2, 0x4446},
	{"Oceanic",  "VT4",                 DC_FAMILY_OCEANIC_ATOM2, 0x4447},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4449},
	{"Beuchat",  "Voyager 2G",          DC_FAMILY_OCEANIC_ATOM2, 0x444B},
	{"Oceanic",  "Atom 3.0",            DC_FAMILY_OCEANIC_ATOM2, 0x444C},
	{"Hollis",   "DG03",                DC_FAMILY_OCEANIC_ATOM2, 0x444D},
	{"Oceanic",  "OCS",                 DC_FAMILY_OCEANIC_ATOM2, 0x4450},
	{"Oceanic",  "OC1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4451},
	{"Oceanic",  "VT 4.1",              DC_FAMILY_OCEANIC_ATOM2, 0x4452},
	{"Aeris",    "Epic",                DC_FAMILY_OCEANIC_ATOM2, 0x4453},
	{"Aeris",    "Elite T3",            DC_FAMILY_OCEANIC_ATOM2, 0x4455},
	{"Oceanic",  "Atom 3.1",            DC_FAMILY_OCEANIC_ATOM2, 0x4456},
	{"Aeris",    "A300 AI",             DC_FAMILY_OCEANIC_ATOM2, 0x4457},
	{"Sherwood", "Wisdom 3",            DC_FAMILY_OCEANIC_ATOM2, 0x4458},
	{"Aeris",    "A300",                DC_FAMILY_OCEANIC_ATOM2, 0x445A},
	{"Hollis",   "TX1",                 DC_FAMILY_OCEANIC_ATOM2, 0x4542},
	{"Sherwood", "Amphos",              DC_FAMILY_OCEANIC_ATOM2, 0x4545},
	{"Sherwood", "Amphos Air",          DC_FAMILY_OCEANIC_ATOM2, 0x4546},
	{"Oceanic",  "Pro Plus 3",          DC_FAMILY_OCEANIC_ATOM2, 0x4548},
	{"Aeris",    "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4549},
	{"Oceanic",  "OCi",                 DC_FAMILY_OCEANIC_ATOM2, 0x454B},
	{"Aeris",    "A300CS",              DC_FAMILY_OCEANIC_ATOM2, 0x454C},
	{"Oceanic",  "F11",                 DC_FAMILY_OCEANIC_ATOM2, 0x4554},
	{"Oceanic",  "VTX",                 DC_FAMILY_OCEANIC_ATOM2, 0x4557},
	/* Mares Nemo */
	{"Mares", "Nemo",         DC_FAMILY_MARES_NEMO, 0},
	{"Mares", "Nemo Steel",   DC_FAMILY_MARES_NEMO, 0},
	{"Mares", "Nemo Titanium",DC_FAMILY_MARES_NEMO, 0},
	{"Mares", "Nemo Excel",   DC_FAMILY_MARES_NEMO, 17},
	{"Mares", "Nemo Apneist", DC_FAMILY_MARES_NEMO, 18},
	/* Mares Puck */
	{"Mares", "Puck",      DC_FAMILY_MARES_PUCK, 7},
	{"Mares", "Puck Air",  DC_FAMILY_MARES_PUCK, 19},
	{"Mares", "Nemo Air",  DC_FAMILY_MARES_PUCK, 4},
	{"Mares", "Nemo Wide", DC_FAMILY_MARES_PUCK, 1},
	/* Mares Darwin */
	{"Mares", "Darwin",     DC_FAMILY_MARES_DARWIN , 0},
	{"Mares", "M1",         DC_FAMILY_MARES_DARWIN , 0},
	{"Mares", "M2",         DC_FAMILY_MARES_DARWIN , 0},
	{"Mares", "Darwin Air", DC_FAMILY_MARES_DARWIN , 1},
	{"Mares", "Airlab",     DC_FAMILY_MARES_DARWIN , 1},
	/* Mares Icon HD */
	{"Mares", "Matrix",            DC_FAMILY_MARES_ICONHD , 0x0F},
	{"Mares", "Smart",             DC_FAMILY_MARES_ICONHD , 0x000010},
	{"Mares", "Smart Apnea",       DC_FAMILY_MARES_ICONHD , 0x010010},
	{"Mares", "Icon HD",           DC_FAMILY_MARES_ICONHD , 0x14},
	{"Mares", "Icon HD Net Ready", DC_FAMILY_MARES_ICONHD , 0x15},
	{"Mares", "Puck Pro",          DC_FAMILY_MARES_ICONHD , 0x18},
	{"Mares", "Nemo Wide 2",       DC_FAMILY_MARES_ICONHD , 0x19},
	{"Mares", "Puck 2",            DC_FAMILY_MARES_ICONHD , 0x1F},
	/* Heinrichs Weikamp */
	{"Heinrichs Weikamp", "OSTC",     DC_FAMILY_HW_OSTC, 0},
	{"Heinrichs Weikamp", "OSTC Mk2", DC_FAMILY_HW_OSTC, 1},
	{"Heinrichs Weikamp", "OSTC 2N",  DC_FAMILY_HW_OSTC, 2},
	{"Heinrichs Weikamp", "OSTC 2C",  DC_FAMILY_HW_OSTC, 3},
	{"Heinrichs Weikamp", "Frog",     DC_FAMILY_HW_FROG, 0},
	{"Heinrichs Weikamp", "OSTC 2",     DC_FAMILY_HW_OSTC3, 0x11},
	{"Heinrichs Weikamp", "OSTC 3",     DC_FAMILY_HW_OSTC3, 0x0A},
	{"Heinrichs Weikamp", "OSTC 3",     DC_FAMILY_HW_OSTC3, 0x1A},
	{"Heinrichs Weikamp", "OSTC cR",    DC_FAMILY_HW_OSTC3, 0x05},
	{"Heinrichs Weikamp", "OSTC Sport", DC_FAMILY_HW_OSTC3, 0x12},
	/* Cressi Edy */
	{"Tusa",   "IQ-700", DC_FAMILY_CRESSI_EDY, 0x05},
	{"Cressi", "Edy",    DC_FAMILY_CRESSI_EDY, 0x08},
	/* Cressi Leonardo */
	{"Cressi", "Leonardo", DC_FAMILY_CRESSI_LEONARDO, 1},
	{"Cressi", "Giotto",   DC_FAMILY_CRESSI_LEONARDO, 4},
	/* Zeagle N2iTiON3 */
	{"Zeagle",   "N2iTiON3",   DC_FAMILY_ZEAGLE_N2ITION3, 0},
	{"Apeks",    "Quantum X",  DC_FAMILY_ZEAGLE_N2ITION3, 0},
	{"Dive Rite", "NiTek Trio", DC_FAMILY_ZEAGLE_N2ITION3, 0},
	{"Scubapro", "XTender 5",  DC_FAMILY_ZEAGLE_N2ITION3, 0},
	/* Atomic Aquatics Cobalt */
#ifdef HAVE_LIBUSB
	{"Atomic Aquatics", "Cobalt", DC_FAMILY_ATOMICS_COBALT, 0},
	{"Atomic Aquatics", "Cobalt 2", DC_FAMILY_ATOMICS_COBALT, 2},
#endif
	/* Shearwater Predator */
	{"Shearwater", "Predator", DC_FAMILY_SHEARWATER_PREDATOR, 2},
	/* Shearwater Petrel */
	{"Shearwater", "Petrel",   DC_FAMILY_SHEARWATER_PETREL, 3},
	{"Shearwater", "Petrel 2", DC_FAMILY_SHEARWATER_PETREL, 3},
	{"Shearwater", "Nerd",     DC_FAMILY_SHEARWATER_PETREL, 3},
	/* Dive Rite NiTek Q */
	{"Dive Rite", "NiTek Q",   DC_FAMILY_DIVERITE_NITEKQ, 0},
	/* Citizen Hyper Aqualand */
	{"Citizen", "Hyper Aqualand", DC_FAMILY_CITIZEN_AQUALAND, 0},
	/* DiveSystem iDive */
	{"DiveSystem", "Orca",          DC_FAMILY_DIVESYSTEM_IDIVE, 0x02},
	{"DiveSystem", "iDive Pro",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x03},
	{"DiveSystem", "iDive DAN",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x04},
	{"DiveSystem", "iDive Tech",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x05},
	{"DiveSystem", "iDive Reb",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x06},
	{"DiveSystem", "iDive Stealth", DC_FAMILY_DIVESYSTEM_IDIVE, 0x07},
	{"DiveSystem", "iDive Free",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x08},
	{"DiveSystem", "iDive Easy",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x09},
	{"DiveSystem", "iDive X3M",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x0A},
	{"DiveSystem", "iDive Deep",    DC_FAMILY_DIVESYSTEM_IDIVE, 0x0B},
	{"DiveSystem", "iX3M Easy",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x22},
	{"DiveSystem", "iX3M Deep",     DC_FAMILY_DIVESYSTEM_IDIVE, 0x23},
	{"DiveSystem", "iX3M Tec",      DC_FAMILY_DIVESYSTEM_IDIVE, 0x24},
	{"DiveSystem", "iX3M Reb",      DC_FAMILY_DIVESYSTEM_IDIVE, 0x25},
};

typedef struct dc_descriptor_iterator_t {
	dc_iterator_t base;
	size_t current;
} dc_descriptor_iterator_t;

static dc_status_t dc_descriptor_iterator_next (dc_iterator_t *iterator, void *item);
static dc_status_t dc_descriptor_iterator_free (dc_iterator_t *iterator);

static const dc_iterator_vtable_t dc_descriptor_iterator_vtable = {
	dc_descriptor_iterator_free,
	dc_descriptor_iterator_next
};

dc_status_t
dc_descriptor_iterator (dc_iterator_t **out)
{
	dc_descriptor_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_descriptor_iterator_t *) malloc (sizeof (dc_descriptor_iterator_t));
	if (iterator == NULL)
		return DC_STATUS_NOMEMORY;

	iterator->base.vtable = &dc_descriptor_iterator_vtable;
	iterator->current = 0;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_descriptor_iterator_free (dc_iterator_t *iterator)
{
	free (iterator);

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
		return DC_TRANSPORT_USB;
	else if (descriptor->type == DC_FAMILY_UWATEC_SMART)
		return DC_TRANSPORT_IRDA;
	else
		return DC_TRANSPORT_SERIAL;
}
