/*
 * libdivecomputer
 *
 * Copyright (C) 2015 Jef Driesen
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

#include <string.h>

#include "common.h"
#include "utils.h"

#ifdef _WIN32
#define DC_TICKS_FORMAT "%I64d"
#else
#define DC_TICKS_FORMAT "%lld"
#endif

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

typedef struct backend_table_t {
	const char *name;
	dc_family_t type;
} backend_table_t;

static const backend_table_t g_backends[] = {
	{"solution",    DC_FAMILY_SUUNTO_SOLUTION},
	{"eon",	        DC_FAMILY_SUUNTO_EON},
	{"vyper",       DC_FAMILY_SUUNTO_VYPER},
	{"vyper2",      DC_FAMILY_SUUNTO_VYPER2},
	{"d9",          DC_FAMILY_SUUNTO_D9},
	{"eonsteel",    DC_FAMILY_SUUNTO_EONSTEEL},
	{"aladin",      DC_FAMILY_UWATEC_ALADIN},
	{"memomouse",   DC_FAMILY_UWATEC_MEMOMOUSE},
	{"smart",       DC_FAMILY_UWATEC_SMART},
	{"meridian",    DC_FAMILY_UWATEC_MERIDIAN},
	{"sensus",      DC_FAMILY_REEFNET_SENSUS},
	{"sensuspro",   DC_FAMILY_REEFNET_SENSUSPRO},
	{"sensusultra", DC_FAMILY_REEFNET_SENSUSULTRA},
	{"vtpro",       DC_FAMILY_OCEANIC_VTPRO},
	{"veo250",      DC_FAMILY_OCEANIC_VEO250},
	{"atom2",       DC_FAMILY_OCEANIC_ATOM2},
	{"nemo",        DC_FAMILY_MARES_NEMO},
	{"puck",        DC_FAMILY_MARES_PUCK},
	{"darwin",      DC_FAMILY_MARES_DARWIN},
	{"iconhd",      DC_FAMILY_MARES_ICONHD},
	{"ostc",        DC_FAMILY_HW_OSTC},
	{"frog",        DC_FAMILY_HW_FROG},
	{"ostc3",       DC_FAMILY_HW_OSTC3},
	{"edy",         DC_FAMILY_CRESSI_EDY},
	{"leonardo",	DC_FAMILY_CRESSI_LEONARDO},
	{"n2ition3",    DC_FAMILY_ZEAGLE_N2ITION3},
	{"cobalt",      DC_FAMILY_ATOMICS_COBALT},
	{"predator",	DC_FAMILY_SHEARWATER_PREDATOR},
	{"petrel",      DC_FAMILY_SHEARWATER_PETREL},
	{"nitekq",      DC_FAMILY_DIVERITE_NITEKQ},
	{"aqualand",    DC_FAMILY_CITIZEN_AQUALAND},
	{"idive",       DC_FAMILY_DIVESYSTEM_IDIVE},
};

const char *
dctool_errmsg (dc_status_t status)
{
	switch (status) {
	case DC_STATUS_SUCCESS:
		return "Success";
	case DC_STATUS_UNSUPPORTED:
		return "Unsupported operation";
	case DC_STATUS_INVALIDARGS:
		return "Invalid arguments";
	case DC_STATUS_NOMEMORY:
		return "Out of memory";
	case DC_STATUS_NODEVICE:
		return "No device found";
	case DC_STATUS_NOACCESS:
		return "Access denied";
	case DC_STATUS_IO:
		return "Input/output error";
	case DC_STATUS_TIMEOUT:
		return "Timeout";
	case DC_STATUS_PROTOCOL:
		return "Protocol error";
	case DC_STATUS_DATAFORMAT:
		return "Data format error";
	case DC_STATUS_CANCELLED:
		return "Cancelled";
	default:
		return "Unknown error";
	}
}

dc_family_t
dctool_family_type (const char *name)
{
	for (unsigned int i = 0; i < C_ARRAY_SIZE (g_backends); ++i) {
		if (strcmp (name, g_backends[i].name) == 0)
			return g_backends[i].type;
	}

	return DC_FAMILY_NULL;
}

const char *
dctool_family_name (dc_family_t type)
{
	for (unsigned int i = 0; i < C_ARRAY_SIZE (g_backends); ++i) {
		if (g_backends[i].type == type)
			return g_backends[i].name;
	}

	return NULL;
}

void
dctool_event_cb (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata)
{
	const dc_event_progress_t *progress = (const dc_event_progress_t *) data;
	const dc_event_devinfo_t *devinfo = (const dc_event_devinfo_t *) data;
	const dc_event_clock_t *clock = (const dc_event_clock_t *) data;
	const dc_event_vendor_t *vendor = (const dc_event_vendor_t *) data;

	switch (event) {
	case DC_EVENT_WAITING:
		message ("Event: waiting for user action\n");
		break;
	case DC_EVENT_PROGRESS:
		message ("Event: progress %3.2f%% (%u/%u)\n",
			100.0 * (double) progress->current / (double) progress->maximum,
			progress->current, progress->maximum);
		break;
	case DC_EVENT_DEVINFO:
		message ("Event: model=%u (0x%08x), firmware=%u (0x%08x), serial=%u (0x%08x)\n",
			devinfo->model, devinfo->model,
			devinfo->firmware, devinfo->firmware,
			devinfo->serial, devinfo->serial);
		break;
	case DC_EVENT_CLOCK:
		message ("Event: systime=" DC_TICKS_FORMAT ", devtime=%u\n",
			clock->systime, clock->devtime);
		break;
	case DC_EVENT_VENDOR:
		message ("Event: vendor=");
		for (unsigned int i = 0; i < vendor->size; ++i)
			message ("%02X", vendor->data[i]);
		message ("\n");
		break;
	default:
		break;
	}
}

dc_status_t
dctool_descriptor_search (dc_descriptor_t **out, const char *name, dc_family_t family, unsigned int model)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	dc_iterator_t *iterator = NULL;
	rc = dc_descriptor_iterator (&iterator);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error creating the device descriptor iterator.");
		return rc;
	}

	dc_descriptor_t *descriptor = NULL, *current = NULL;
	while ((rc = dc_iterator_next (iterator, &descriptor)) == DC_STATUS_SUCCESS) {
		if (name) {
			const char *vendor = dc_descriptor_get_vendor (descriptor);
			const char *product = dc_descriptor_get_product (descriptor);

			size_t n = strlen (vendor);
			if (strncasecmp (name, vendor, n) == 0 && name[n] == ' ' &&
				strcasecmp (name + n + 1, product) == 0)
			{
				current = descriptor;
				break;
			} else if (strcasecmp (name, product) == 0) {
				current = descriptor;
				break;
			}
		} else {
			if (family == dc_descriptor_get_type (descriptor)) {
				if (model == dc_descriptor_get_model (descriptor)) {
					// Exact match found. Return immediately.
					dc_descriptor_free (current);
					current = descriptor;
					break;
				} else {
					// Possible match found. Keep searching for an exact match.
					// If no exact match is found, the first match is returned.
					if (current == NULL) {
						current = descriptor;
						descriptor = NULL;
					}
				}
			}
		}

		dc_descriptor_free (descriptor);
	}

	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_DONE) {
		dc_descriptor_free (current);
		dc_iterator_free (iterator);
		ERROR ("Error iterating the device descriptors.");
		return rc;
	}

	dc_iterator_free (iterator);

	*out = current;

	return DC_STATUS_SUCCESS;
}
