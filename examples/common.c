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
#include <stdio.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

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
	unsigned int model;
} backend_table_t;

static const backend_table_t g_backends[] = {
	{"solution",    DC_FAMILY_SUUNTO_SOLUTION,     0},
	{"eon",	        DC_FAMILY_SUUNTO_EON,          0},
	{"vyper",       DC_FAMILY_SUUNTO_VYPER,        0x0A},
	{"vyper2",      DC_FAMILY_SUUNTO_VYPER2,       0x10},
	{"d9",          DC_FAMILY_SUUNTO_D9,           0x0E},
	{"eonsteel",    DC_FAMILY_SUUNTO_EONSTEEL,     0},
	{"aladin",      DC_FAMILY_UWATEC_ALADIN,       0x3F},
	{"memomouse",   DC_FAMILY_UWATEC_MEMOMOUSE,    0},
	{"smart",       DC_FAMILY_UWATEC_SMART,        0x10},
	{"meridian",    DC_FAMILY_UWATEC_MERIDIAN,     0x20},
	{"sensus",      DC_FAMILY_REEFNET_SENSUS,      1},
	{"sensuspro",   DC_FAMILY_REEFNET_SENSUSPRO,   2},
	{"sensusultra", DC_FAMILY_REEFNET_SENSUSULTRA, 3},
	{"vtpro",       DC_FAMILY_OCEANIC_VTPRO,       0x4245},
	{"veo250",      DC_FAMILY_OCEANIC_VEO250,      0x424C},
	{"atom2",       DC_FAMILY_OCEANIC_ATOM2,       0x4342},
	{"nemo",        DC_FAMILY_MARES_NEMO,          0},
	{"puck",        DC_FAMILY_MARES_PUCK,          7},
	{"darwin",      DC_FAMILY_MARES_DARWIN,        0},
	{"iconhd",      DC_FAMILY_MARES_ICONHD,        0x14},
	{"ostc",        DC_FAMILY_HW_OSTC,             0},
	{"frog",        DC_FAMILY_HW_FROG,             0},
	{"ostc3",       DC_FAMILY_HW_OSTC3,            0x0A},
	{"edy",         DC_FAMILY_CRESSI_EDY,          0x08},
	{"leonardo",	DC_FAMILY_CRESSI_LEONARDO,     1},
	{"n2ition3",    DC_FAMILY_ZEAGLE_N2ITION3,     0},
	{"cobalt",      DC_FAMILY_ATOMICS_COBALT,      0},
	{"predator",	DC_FAMILY_SHEARWATER_PREDATOR, 2},
	{"petrel",      DC_FAMILY_SHEARWATER_PETREL,   3},
	{"nitekq",      DC_FAMILY_DIVERITE_NITEKQ,     0},
	{"aqualand",    DC_FAMILY_CITIZEN_AQUALAND,    0},
	{"idive",       DC_FAMILY_DIVESYSTEM_IDIVE,    0x03},
	{"cochran",     DC_FAMILY_COCHRAN_COMMANDER,   0},
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

unsigned int
dctool_family_model (dc_family_t type)
{
	for (unsigned int i = 0; i < C_ARRAY_SIZE (g_backends); ++i) {
		if (g_backends[i].type == type)
			return g_backends[i].model;
	}

	return 0;
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

static unsigned char
hex2dec (unsigned char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	else if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	else if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	else
		return 0;
}

dc_buffer_t *
dctool_convert_hex2bin (const char *str)
{
	// Get the length of the fingerprint data.
	size_t nbytes = (str ? strlen (str) / 2 : 0);
	if (nbytes == 0)
		return NULL;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (nbytes);

	// Convert the hexadecimal string.
	for (unsigned int i = 0; i < nbytes; ++i) {
		unsigned char msn = hex2dec (str[i * 2 + 0]);
		unsigned char lsn = hex2dec (str[i * 2 + 1]);
		unsigned char byte = (msn << 4) + lsn;

		dc_buffer_append (buffer, &byte, 1);
	}

	return buffer;
}

void
dctool_file_write (const char *filename, dc_buffer_t *buffer)
{
	FILE *fp = NULL;

	// Open the file.
	if (filename) {
		fp = fopen (filename, "wb");
	} else {
		fp = stdout;
#ifdef _WIN32
		// Change from text mode to binary mode.
		_setmode (_fileno (fp), _O_BINARY);
#endif
	}
	if (fp == NULL)
		return;

	// Write the entire buffer to the file.
	fwrite (dc_buffer_get_data (buffer), 1, dc_buffer_get_size (buffer), fp);

	// Close the file.
	fclose (fp);
}

dc_buffer_t *
dctool_file_read (const char *filename)
{
	FILE *fp = NULL;

	// Open the file.
	if (filename) {
		fp = fopen (filename, "rb");
	} else {
		fp = stdin;
#ifdef _WIN32
		// Change from text mode to binary mode.
		_setmode (_fileno (fp), _O_BINARY);
#endif
	}
	if (fp == NULL)
		return NULL;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);

	// Read the entire file into the buffer.
	size_t n = 0;
	unsigned char block[1024] = {0};
	while ((n = fread (block, 1, sizeof (block), fp)) > 0) {
		dc_buffer_append (buffer, block, n);
	}

	// Close the file.
	fclose (fp);

	return buffer;
}
