/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <stdlib.h>
#include <assert.h>

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "parser-private.h"
#include "array.h"
#include "units.h"
#include "utils.h"

typedef struct oceanic_atom2_parser_t oceanic_atom2_parser_t;

struct oceanic_atom2_parser_t {
	parser_t base;
};

static parser_status_t oceanic_atom2_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t oceanic_atom2_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t oceanic_atom2_parser_destroy (parser_t *abstract);

static const parser_backend_t oceanic_atom2_parser_backend = {
	PARSER_TYPE_OCEANIC_ATOM2,
	oceanic_atom2_parser_set_data, /* set_data */
	oceanic_atom2_parser_samples_foreach, /* samples_foreach */
	oceanic_atom2_parser_destroy /* destroy */
};


static int
parser_is_oceanic_atom2 (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_atom2_parser_backend;
}


parser_status_t
oceanic_atom2_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) malloc (sizeof (oceanic_atom2_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &oceanic_atom2_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_destroy (parser_t *abstract)
{
	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 11 * PAGESIZE / 2)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;
	unsigned interval = 0;
	switch (data[0x17] & 0x03) {
	case 0:
		interval = 2;
		break;
	case 1:
		interval = 15;
		break;
	case 2:
		interval = 30;
		break;
	case 3:
		interval = 60;
		break;
	}

	int complete = 1;

	unsigned int tank = 0;
	unsigned int pressure = data[0x42] + (data[0x42 + 1] << 8);
	unsigned int temperature = data[0x47];

	unsigned int offset = 9 * PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		parser_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Time.
		if (complete) {
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
		}

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_ATOM2;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);

		// Check for a tank switch sample.
		if (data[offset + 0] == 0xAA) {
			// Tank Number (one based index)
			tank = (data[offset + 1] & 0x03) - 1;

			// Tank Pressure (2 psi)
			pressure = (((data[offset + 4] << 8) + data[offset + 5]) & 0x0FFF) * 2;

			complete = 0;
		} else {
			// Temperature (°F)
			if (data[offset + 0] & 0x80)
				temperature += (data[offset + 7] & 0xFC) >> 2;
			else
				temperature -= (data[offset + 7] & 0xFC) >> 2;
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

			// Tank Pressure (psi)
			pressure -= data[offset + 1];
			sample.pressure.tank = tank;
			sample.pressure.value = pressure * PSI / BAR;
			if (callback && pressure != 10000) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

			// Depth (1/16 ft)
			unsigned int depth = (data[offset + 2] + (data[offset + 3] << 8)) & 0x0FFF;
			sample.depth = depth / 16.0 * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			complete = 1;
		}

		offset += PAGESIZE / 2;
	}

	return PARSER_STATUS_SUCCESS;
}
