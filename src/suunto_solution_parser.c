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

#include <libdivecomputer/units.h>

#include "suunto_solution.h"
#include "context-private.h"
#include "parser-private.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &suunto_solution_parser_vtable)

typedef struct suunto_solution_parser_t suunto_solution_parser_t;

struct suunto_solution_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
};

static dc_status_t suunto_solution_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t suunto_solution_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t suunto_solution_parser_vtable = {
	sizeof(suunto_solution_parser_t),
	DC_FAMILY_SUUNTO_SOLUTION,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	NULL, /* datetime */
	suunto_solution_parser_get_field, /* fields */
	suunto_solution_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
suunto_solution_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	suunto_solution_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (suunto_solution_parser_t *) dc_parser_allocate (context, &suunto_solution_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_solution_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_solution_parser_t *parser = (suunto_solution_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 4)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		unsigned int nsamples = 0;
		unsigned int depth = 0, maxdepth = 0;
		unsigned int offset = 3;
		while (offset < size && data[offset] != 0x80) {
			unsigned char raw = data[offset++];
			if (raw < 0x7e || raw > 0x82) {
				depth += (signed char) raw;
				if (raw == 0x7D || raw == 0x83) {
					if (offset + 1 > size)
						return DC_STATUS_DATAFORMAT;
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
			return DC_STATUS_DATAFORMAT;

		parser->cached = 1;
		parser->divetime = (nsamples * 3 + data[marker + 1]) * 60;
		parser->maxdepth = maxdepth;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			gasmix->oxygen = 0.21;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_solution_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int time = 0, depth = 0;
	unsigned int gasmix_previous = 0xFFFFFFFF;
	unsigned int gasmix = 0;

	unsigned int offset = 3;
	while (offset < size &&	data[offset] != 0x80) {
		dc_sample_value_t sample = {0};
		unsigned char value = data[offset++];
		if (value < 0x7e || value > 0x82) {
			// Time (minutes).
			time += 3 * 60;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Depth (ft).
			depth += (signed char) value;
			if (value == 0x7D || value == 0x83) {
				// A value of 0x7D (125) or 0x83 (-125) indicates a descent
				// or ascent greater than 124 feet. The remaining part of
				// the total delta value is stored in the next byte.
				if (offset + 1 > size)
					return DC_STATUS_DATAFORMAT;
				depth += (signed char) data[offset++];
			}
			sample.depth = depth * FEET;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Gas change.
			if (gasmix != gasmix_previous) {
				sample.gasmix = gasmix;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix_previous = gasmix;
			}
		} else {
			// Event.
			sample.event.type = SAMPLE_EVENT_NONE;
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
				WARNING (abstract->context, "Unknown event");
				break;
			}

			if (sample.event.type != SAMPLE_EVENT_NONE) {
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			}
		}
	}

	if (data[offset] != 0x80)
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}
