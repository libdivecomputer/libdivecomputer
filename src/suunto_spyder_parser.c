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

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

typedef struct suunto_spyder_parser_t suunto_spyder_parser_t;

struct suunto_spyder_parser_t {
	parser_t base;
};

static parser_status_t suunto_spyder_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t suunto_spyder_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t suunto_spyder_parser_destroy (parser_t *abstract);

static const parser_backend_t suunto_spyder_parser_backend = {
	PARSER_TYPE_SUUNTO_SPYDER,
	suunto_spyder_parser_set_data, /* set_data */
	suunto_spyder_parser_samples_foreach, /* samples_foreach */
	suunto_spyder_parser_destroy /* destroy */
};


static int
parser_is_suunto_spyder (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_spyder_parser_backend;
}


parser_status_t
suunto_spyder_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	suunto_spyder_parser_t *parser = malloc (sizeof (suunto_spyder_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &suunto_spyder_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_spyder_parser_destroy (parser_t *abstract)
{
	if (! parser_is_suunto_spyder (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_spyder_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_suunto_spyder (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_spyder_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_suunto_spyder (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 13)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0, depth = 0;
	unsigned int interval = data[3];

	unsigned int offset = 11;
	while (offset < size && data[offset] != 0x80) {
		parser_sample_value_t sample = {0};
		unsigned char value = data[offset++];
		if (value < 0x7d || value > 0x82) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			// Depth (ft).
			depth += (signed char) value;
			sample.depth = depth * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
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

	return PARSER_STATUS_SUCCESS;
}
