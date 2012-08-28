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

#include <libdivecomputer/atomics_cobalt.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define SZ_HEADER       228
#define SZ_GASMIX       18
#define SZ_GASSWITCH    6
#define SZ_SEGMENT      16

typedef struct atomics_cobalt_parser_t atomics_cobalt_parser_t;

struct atomics_cobalt_parser_t {
	dc_parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
};

static dc_status_t atomics_cobalt_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t atomics_cobalt_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t atomics_cobalt_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t atomics_cobalt_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t atomics_cobalt_parser_destroy (dc_parser_t *abstract);

static const parser_backend_t atomics_cobalt_parser_backend = {
	DC_FAMILY_ATOMICS_COBALT,
	atomics_cobalt_parser_set_data, /* set_data */
	atomics_cobalt_parser_get_datetime, /* datetime */
	atomics_cobalt_parser_get_field, /* fields */
	atomics_cobalt_parser_samples_foreach, /* samples_foreach */
	atomics_cobalt_parser_destroy /* destroy */
};


static int
parser_is_atomics_cobalt (dc_parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &atomics_cobalt_parser_backend;
}


dc_status_t
atomics_cobalt_parser_create (dc_parser_t **out, dc_context_t *context)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) malloc (sizeof (atomics_cobalt_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &atomics_cobalt_parser_backend);

	// Set the default values.
	parser->atmospheric = 0.0;
	parser->hydrostatic = 1025.0 * GRAVITY;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_destroy (dc_parser_t *abstract)
{
	if (! parser_is_atomics_cobalt (abstract))
		return DC_STATUS_INVALIDARGS;

	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_atomics_cobalt (abstract))
		return DC_STATUS_INVALIDARGS;

	return DC_STATUS_SUCCESS;
}


dc_status_t
atomics_cobalt_parser_set_calibration (dc_parser_t *abstract, double atmospheric, double hydrostatic)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t*) abstract;

	if (! parser_is_atomics_cobalt (abstract))
		return DC_STATUS_INVALIDARGS;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = array_uint16_le (p + 0x14);
		datetime->month  = p[0x16];
		datetime->day    = p[0x17];
		datetime->hour   = p[0x18];
		datetime->minute = p[0x19];
		datetime->second = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	double atmospheric = 0.0;
	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (p + 0x26) * BAR / 1000.0;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (p + 0x58) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (array_uint16_le (p + 0x56) * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = p[0x2a];
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = p[SZ_HEADER + SZ_GASMIX * flags + 5] / 100.0;
			gasmix->oxygen = p[SZ_HEADER + SZ_GASMIX * flags + 4] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	atomics_cobalt_parser_t *parser = (atomics_cobalt_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int interval = data[0x1a];
	unsigned int ngasmixes = data[0x2a];
	unsigned int nswitches = data[0x2b];
	unsigned int nsegments = array_uint16_le (data + 0x50);

	unsigned int header = SZ_HEADER + SZ_GASMIX * ngasmixes +
		SZ_GASSWITCH * nswitches;

	if (size < header + SZ_SEGMENT * nsegments)
		return DC_STATUS_DATAFORMAT;

	double atmospheric = 0.0;
	if (parser->atmospheric)
		atmospheric = parser->atmospheric;
	else
		atmospheric = array_uint16_le (data + 0x26) * BAR / 1000.0;

	unsigned int time = 0;
	unsigned int offset = header;
	while (offset + SZ_SEGMENT <= size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/1000 bar).
		unsigned int depth = array_uint16_le (data + offset + 0);
		sample.depth = (depth * BAR / 1000.0 - atmospheric) / parser->hydrostatic;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Pressure (1 psi).
		unsigned int pressure = array_uint16_le (data + offset + 2);
		sample.pressure.tank = 0;
		sample.pressure.value = pressure * PSI / BAR;
		if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);

		// Temperature (1 °F).
		unsigned int temperature = data[offset + 8];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		offset += SZ_SEGMENT;
	}

	return DC_STATUS_SUCCESS;
}
