/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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

#include "atomics_cobalt.h"
#include "parser-private.h"
#include "utils.h"
#include "array.h"
#include "units.h"

#define SZ_HEADER       228
#define SZ_GASMIX       18
#define SZ_GASSWITCH    6
#define SZ_SEGMENT      16

typedef struct atomics_cobalt_parser_t atomics_cobalt_parser_t;

struct atomics_cobalt_parser_t {
	parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
};

static parser_status_t atomics_cobalt_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t atomics_cobalt_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t atomics_cobalt_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t atomics_cobalt_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t atomics_cobalt_parser_destroy (parser_t *abstract);

static const parser_backend_t atomics_cobalt_parser_backend = {
	PARSER_TYPE_ATOMICS_COBALT,
	atomics_cobalt_parser_set_data, /* set_data */
	atomics_cobalt_parser_get_datetime, /* datetime */
	atomics_cobalt_parser_get_field, /* fields */
	atomics_cobalt_parser_samples_foreach, /* samples_foreach */
	atomics_cobalt_parser_destroy /* destroy */
};


static int
parser_is_atomics_cobalt (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &atomics_cobalt_parser_backend;
}


parser_status_t
atomics_cobalt_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) malloc (sizeof (atomics_cobalt_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &atomics_cobalt_parser_backend);

	// Set the default values.
	parser->atmospheric = 0.0;
	parser->hydrostatic = 1025.0 * GRAVITY;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
atomics_cobalt_parser_destroy (parser_t *abstract)
{
	if (! parser_is_atomics_cobalt (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
atomics_cobalt_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_atomics_cobalt (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


parser_status_t
atomics_cobalt_parser_set_calibration (parser_t *abstract, double atmospheric, double hydrostatic)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t*) abstract;

	if (! parser_is_atomics_cobalt (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
atomics_cobalt_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = array_uint16_le (p + 0x14);
		datetime->month  = p[0x16];
		datetime->day    = p[0x17];
		datetime->hour   = p[0x18];
		datetime->minute = p[0x19];
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
atomics_cobalt_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	if (abstract->size < SZ_HEADER)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	gasmix_t *gasmix = (gasmix_t *) value;

	double atmospheric = 0.0;
	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (p + 0x26) * BAR / 1000.0;

	if (value) {
		switch (type) {
		case FIELD_TYPE_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (p + 0x58) * 60;
			break;
		case FIELD_TYPE_MAXDEPTH:
			*((double *) value) = (array_uint16_le (p + 0x56) * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
			break;
		case FIELD_TYPE_GASMIX_COUNT:
			*((unsigned int *) value) = p[0x2a];
			break;
		case FIELD_TYPE_GASMIX:
			gasmix->helium = p[SZ_HEADER + SZ_GASMIX * flags + 5] / 100.0;
			gasmix->oxygen = p[SZ_HEADER + SZ_GASMIX * flags + 4] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return PARSER_STATUS_UNSUPPORTED;
		}
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
atomics_cobalt_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return PARSER_STATUS_ERROR;

	unsigned int interval = data[0x1a];
	unsigned int ngasmixes = data[0x2a];
	unsigned int nswitches = data[0x2b];
	unsigned int nsegments = array_uint16_le (data + 0x50);

	unsigned int header = SZ_HEADER + SZ_GASMIX * ngasmixes +
		SZ_GASSWITCH * nswitches;

	if (size < header + SZ_SEGMENT * nsegments)
		return PARSER_STATUS_ERROR;

	double atmospheric = 0.0;
	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (data + 0x26) * BAR / 1000.0;

	unsigned int time = 0;
	unsigned int offset = header;
	while (offset + SZ_SEGMENT <= size) {
		parser_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Depth (1/1000 bar).
		unsigned int depth = array_uint16_le (data + offset + 0);
		sample.depth = (depth * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

		// Pressure (1 psi).
		unsigned int pressure = array_uint16_le (data + offset + 2);
		sample.pressure.tank = 0;
		sample.pressure.value = pressure * PSI / BAR;
		if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

		// Temperature (1 °F).
		unsigned int temperature = data[offset + 8];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

		offset += SZ_SEGMENT;
	}

	return PARSER_STATUS_SUCCESS;
}
