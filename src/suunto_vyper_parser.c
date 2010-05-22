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
#include <assert.h>

#include "suunto_vyper.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"

typedef struct suunto_vyper_parser_t suunto_vyper_parser_t;

struct suunto_vyper_parser_t {
	parser_t base;
};

static parser_status_t suunto_vyper_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t suunto_vyper_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t suunto_vyper_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t suunto_vyper_parser_destroy (parser_t *abstract);

static const parser_backend_t suunto_vyper_parser_backend = {
	PARSER_TYPE_SUUNTO_VYPER,
	suunto_vyper_parser_set_data, /* set_data */
	suunto_vyper_parser_get_datetime, /* datetime */
	suunto_vyper_parser_samples_foreach, /* samples_foreach */
	suunto_vyper_parser_destroy /* destroy */
};


static int
parser_is_suunto_vyper (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_vyper_parser_backend;
}


parser_status_t
suunto_vyper_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	suunto_vyper_parser_t *parser = (suunto_vyper_parser_t *) malloc (sizeof (suunto_vyper_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &suunto_vyper_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_vyper_parser_destroy (parser_t *abstract)
{
	if (! parser_is_suunto_vyper (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_vyper_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_suunto_vyper (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_vyper_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 9 + 5)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data + 9;

	if (datetime) {
		datetime->year   = p[0] + (p[0] < 90 ? 2000 : 1900);
		datetime->month  = p[1];
		datetime->day    = p[2];
		datetime->hour   = p[3];
		datetime->minute = p[4];
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_vyper_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_suunto_vyper (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 18)
		return PARSER_STATUS_ERROR;

	// Find the maximum depth.
	unsigned int depth = 0, maxdepth = 0;
	unsigned int offset = 14;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];
		if (value < 0x79 || value > 0x87) {
			depth += (signed char) value;
			if (depth > maxdepth)
				maxdepth = depth;
		}
	}

	// Store the offset to the end marker.
	unsigned int marker = offset;
	if (marker + 4 >= size || data[marker] != 0x80)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;
	unsigned int interval = data[3];
	unsigned int complete = 1;

	parser_sample_value_t sample = {0};

	// Time
	sample.time = time;
	if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

	// Temperature (°C)
	sample.temperature = (signed char) data[8];
	if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

	// Tank Pressure (2 bar)
	sample.pressure.tank = 0;
	sample.pressure.value = data[5] * 2;
	if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

	depth = 0;
	offset = 14;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];

		if (complete) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
			complete = 0;
		}

		if (value < 0x79 || value > 0x87) {
			// Delta depth.
			depth += (signed char) value;

			// Temperature at maximum depth (°C)
			if (depth == maxdepth) {
				sample.temperature = (signed char) data[marker + 1];
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
			case 0x7a: // Slow
				sample.event.type = SAMPLE_EVENT_ASCENT;
				break;
			case 0x7b: // Violation
				sample.event.type = SAMPLE_EVENT_VIOLATION;
				break;
			case 0x7c: // Bookmark
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				break;
			case 0x7d: // Surface
				sample.event.type = SAMPLE_EVENT_SURFACE;
				break;
			case 0x7e: // Deco
				sample.event.type = SAMPLE_EVENT_DECOSTOP;
				break;
			case 0x7f: // Ceiling (Deco Violation)
				sample.event.type = SAMPLE_EVENT_CEILING;
				break;
			case 0x81: // Safety Stop
				sample.event.type = SAMPLE_EVENT_SAFETYSTOP;
				break;
			case 0x87: // Gas Change
				assert (offset < size);
				sample.event.type = SAMPLE_EVENT_GASCHANGE;
				sample.event.value = data[offset++];
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

	// Temperature (°C)
	sample.temperature = (signed char) data[offset + 2];
	if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

	// Tank Pressure (2 bar)
	sample.pressure.tank = 0;
	sample.pressure.value = data[offset + 3] * 2;
	if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

	return PARSER_STATUS_SUCCESS;
}
