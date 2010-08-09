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

#include "oceanic_veo250.h"
#include "oceanic_common.h"
#include "parser-private.h"
#include "array.h"
#include "units.h"
#include "utils.h"

typedef struct oceanic_veo250_parser_t oceanic_veo250_parser_t;

struct oceanic_veo250_parser_t {
	parser_t base;
	unsigned int model;
};

static parser_status_t oceanic_veo250_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t oceanic_veo250_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t oceanic_veo250_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t oceanic_veo250_parser_destroy (parser_t *abstract);

static const parser_backend_t oceanic_veo250_parser_backend = {
	PARSER_TYPE_OCEANIC_VEO250,
	oceanic_veo250_parser_set_data, /* set_data */
	oceanic_veo250_parser_get_datetime, /* datetime */
	oceanic_veo250_parser_samples_foreach, /* samples_foreach */
	oceanic_veo250_parser_destroy /* destroy */
};


static int
parser_is_oceanic_veo250 (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_veo250_parser_backend;
}


parser_status_t
oceanic_veo250_parser_create (parser_t **out, unsigned int model)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) malloc (sizeof (oceanic_veo250_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &oceanic_veo250_parser_backend);

	// Set the default values.
	parser->model = model;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_veo250_parser_destroy (parser_t *abstract)
{
	if (! parser_is_oceanic_veo250 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_veo250_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_oceanic_veo250 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_veo250_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) abstract;

	if (abstract->size < 8)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = ((p[5] & 0xF0) >> 4) + ((p[1] & 0xE0) >> 1) + 2000;
		datetime->month  = ((p[7] & 0xF0) >> 4);
		datetime->day    = p[1] & 0x1F;
		datetime->hour   = p[3];
		datetime->minute = p[2];
		datetime->second = 0;

		if (parser->model == 0x424B || parser->model == 0x424C)
			datetime->year += 3;
	}

	return PARSER_STATUS_SUCCESS;
}

static parser_status_t
oceanic_veo250_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_oceanic_veo250 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;
	unsigned int interval = 0;
	switch (data[0x27] & 0x03) {
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

	unsigned int offset = 5 * PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		parser_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Time.
		time += interval;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_VEO250;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);

		// Depth (ft)
		unsigned int depth = data[offset + 2];
		sample.depth = depth * FEET;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

		// Temperature (°F)
		unsigned int temperature = data[offset + 7];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

		offset += PAGESIZE / 2;
	}

	return PARSER_STATUS_SUCCESS;
}
