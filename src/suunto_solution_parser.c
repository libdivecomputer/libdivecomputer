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

#include "suunto_solution.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"

typedef struct suunto_solution_parser_t suunto_solution_parser_t;

struct suunto_solution_parser_t {
	parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
};

static parser_status_t suunto_solution_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t suunto_solution_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t suunto_solution_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t suunto_solution_parser_destroy (parser_t *abstract);

static const parser_backend_t suunto_solution_parser_backend = {
	PARSER_TYPE_SUUNTO_SOLUTION,
	suunto_solution_parser_set_data, /* set_data */
	NULL, /* datetime */
	suunto_solution_parser_get_field, /* fields */
	suunto_solution_parser_samples_foreach, /* samples_foreach */
	suunto_solution_parser_destroy /* destroy */
};


static int
parser_is_suunto_solution (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_solution_parser_backend;
}


parser_status_t
suunto_solution_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	suunto_solution_parser_t *parser = (suunto_solution_parser_t *) malloc (sizeof (suunto_solution_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &suunto_solution_parser_backend);

	// Set the default values.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_solution_parser_destroy (parser_t *abstract)
{
	if (! parser_is_suunto_solution (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_solution_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	suunto_solution_parser_t *parser = (suunto_solution_parser_t *) abstract;

	if (! parser_is_suunto_solution (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_solution_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	suunto_solution_parser_t *parser = (suunto_solution_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 4)
		return PARSER_STATUS_ERROR;

	if (!parser->cached) {
		unsigned int nsamples = 0;
		unsigned int depth = 0, maxdepth = 0;
		unsigned int offset = 3;
		while (offset < size && data[offset] != 0x80) {
			unsigned char value = data[offset++];
			if (value < 0x7e || value > 0x82) {
				depth += (signed char) value;
				if (value == 0x7D || value == 0x83) {
					if (offset + 1 > size)
						return PARSER_STATUS_ERROR;
					depth += (signed char) data[offset++];
				}
				if (depth > maxdepth)
					maxdepth = depth;
				nsamples++;
			}
		}

		// Store the offset to the end marker.
		unsigned int marker = offset;
		if (marker + 1 >= size || data[marker] != 0x80)
			return PARSER_STATUS_ERROR;

		parser->cached = 1;
		parser->divetime = (nsamples * 3 + data[marker + 1]) * 60;
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
suunto_solution_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_suunto_solution (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 4)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0, depth = 0;

	unsigned int offset = 3;
	while (offset < size &&	data[offset] != 0x80) {
		parser_sample_value_t sample = {0};
		unsigned char value = data[offset++];
		if (value < 0x7e || value > 0x82) {
			// Time (minutes).
			time += 3 * 60;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			// Depth (ft).
			depth += (signed char) value;
			if (value == 0x7D || value == 0x83) {
				// A value of 0x7D (125) or 0x83 (-125) indicates a descent
				// or ascent greater than 124 feet. The remaining part of
				// the total delta value is stored in the next byte.
				if (offset + 1 > size)
					return PARSER_STATUS_ERROR;
				depth += (signed char) data[offset++];
			}
			sample.depth = depth * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
		} else {
			// Event.
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = 0;
			switch (value) {
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

	if (data[offset] != 0x80)
		return PARSER_STATUS_ERROR;

	return PARSER_STATUS_SUCCESS;
}
