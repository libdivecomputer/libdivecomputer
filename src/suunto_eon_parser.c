/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
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

#include "suunto_eon.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

typedef struct suunto_eon_parser_t suunto_eon_parser_t;

struct suunto_eon_parser_t {
	parser_t base;
	int spyder;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
};

static parser_status_t suunto_eon_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t suunto_eon_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t suunto_eon_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t suunto_eon_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t suunto_eon_parser_destroy (parser_t *abstract);

static const parser_backend_t suunto_eon_parser_backend = {
	PARSER_TYPE_SUUNTO_EON,
	suunto_eon_parser_set_data, /* set_data */
	suunto_eon_parser_get_datetime, /* datetime */
	suunto_eon_parser_get_field, /* fields */
	suunto_eon_parser_samples_foreach, /* samples_foreach */
	suunto_eon_parser_destroy /* destroy */
};


static int
parser_is_suunto_eon (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_eon_parser_backend;
}


parser_status_t
suunto_eon_parser_create (parser_t **out, int spyder)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) malloc (sizeof (suunto_eon_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &suunto_eon_parser_backend);

	// Set the default values.
	parser->spyder = spyder;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_eon_parser_destroy (parser_t *abstract)
{
	if (! parser_is_suunto_eon (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_eon_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	if (! parser_is_suunto_eon (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_eon_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	if (abstract->size < 6 + 5)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data + 6;

	if (datetime) {
		if (parser->spyder) {
			datetime->year   = p[0] + (p[0] < 90 ? 2000 : 1900);
			datetime->month  = p[1];
			datetime->day    = p[2];
			datetime->hour   = p[3];
			datetime->minute = p[4];
		} else {
			datetime->year   = bcd2dec (p[0]) + (bcd2dec (p[0]) < 85 ? 2000 : 1900);
			datetime->month  = bcd2dec (p[1]);
			datetime->day    = bcd2dec (p[2]);
			datetime->hour   = bcd2dec (p[3]);
			datetime->minute = bcd2dec (p[4]);
		}
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_eon_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 13)
		return PARSER_STATUS_ERROR;

	if (!parser->cached) {
		unsigned int interval = data[3];
		unsigned int nsamples = 0;
		unsigned int depth = 0, maxdepth = 0;
		unsigned int offset = 11;
		while (offset < size && data[offset] != 0x80) {
			unsigned char value = data[offset++];
			if (value < 0x7d || value > 0x82) {
				depth += (signed char) value;
				if (depth > maxdepth)
					maxdepth = depth;
				nsamples++;
			}
		}

		// Store the offset to the end marker.
		unsigned int marker = offset;
		if (marker + 2 >= size || data[marker] != 0x80)
			return PARSER_STATUS_ERROR;

		parser->cached = 1;
		parser->divetime = nsamples * interval;
		parser->maxdepth = maxdepth;
	}

	gasmix_t *gasmix = (gasmix_t *) value;

	if (value) {
		switch (type) {
		case FIELD_TYPE_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case FIELD_TYPE_MAXDEPTH:
			*((double *) value) = parser->maxdepth * FEET;
			break;
		case FIELD_TYPE_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case FIELD_TYPE_GASMIX:
			gasmix->helium = 0.0;
			if ((data[4] & 0x80) && !parser->spyder)
				gasmix->oxygen = data[0x05] / 100.0;
			else
				gasmix->oxygen = 0.21;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return PARSER_STATUS_UNSUPPORTED;
		}
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_eon_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	if (! parser_is_suunto_eon (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 13)
		return PARSER_STATUS_ERROR;

	// Find the maximum depth.
	unsigned int depth = 0, maxdepth = 0;
	unsigned int offset = 11;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];
		if (value < 0x7d || value > 0x82) {
			depth += (signed char) value;
			if (depth > maxdepth)
				maxdepth = depth;
		}
	}

	// Store the offset to the end marker.
	unsigned int marker = offset;
	if (marker + 2 >= size || data[marker] != 0x80)
		return PARSER_STATUS_ERROR;

	// The Solution Nitrox/Vario stores nitrox data, not tank pressure.
	unsigned int nitrox = !parser->spyder && (data[4] & 0x80);

	unsigned int time = 0;
	unsigned int interval = data[3];
	unsigned int complete = 1;

	parser_sample_value_t sample = {0};

	// Time
	sample.time = time;
	if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

	// Tank Pressure (2 bar)
	if (!nitrox) {
		sample.pressure.tank = 0;
		sample.pressure.value = data[5] * 2;
		if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
	}

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

	depth = 0;
	offset = 11;
	while (offset < size && data[offset] != 0x80) {
		parser_sample_value_t sample = {0};
		unsigned char value = data[offset++];

		if (complete) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
			complete = 0;
		}

		if (value < 0x7d || value > 0x82) {
			// Delta depth.
			depth += (signed char) value;

			// Temperature at maximum depth (°C)
			if (depth == maxdepth) {
				if (parser->spyder)
					sample.temperature = (signed char) data[marker + 1];
				else
					sample.temperature = data[marker + 1] - 40;
				if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
			}

			// Depth (ft).
			sample.depth = depth * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			complete = 1;
		} else {
			// Event.
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = 0;
			switch (value) {
			case 0x7d: // Surface
				sample.event.type = SAMPLE_EVENT_SURFACE;
				break;
			case 0x7e: // Deco, ASC
				sample.event.type = SAMPLE_EVENT_DECOSTOP;
				break;
			case 0x7f: // Ceiling, ERR
				sample.event.type = SAMPLE_EVENT_CEILING;
				break;
			case 0x81: // Slow
				sample.event.type = SAMPLE_EVENT_ASCENT;
				break;
			default: // Unknown
				WARNING ("Unknown event");
				break;
			}

			if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
		}
	}

	// Time
	if (complete) {
		time += interval;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
	}

	// Tank Pressure (2 bar)
	if (!nitrox) {
		sample.pressure.tank = 0;
		sample.pressure.value = data[offset + 2] * 2;
		if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
	}

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

	return PARSER_STATUS_SUCCESS;
}
