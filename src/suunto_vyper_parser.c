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

#include "suunto_vyper.h"
#include "context-private.h"
#include "parser-private.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &suunto_vyper_parser_vtable)

#define NGASMIXES 3

typedef struct suunto_vyper_parser_t suunto_vyper_parser_t;

struct suunto_vyper_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int marker;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
};

static dc_status_t suunto_vyper_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t suunto_vyper_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t suunto_vyper_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t suunto_vyper_parser_vtable = {
	sizeof(suunto_vyper_parser_t),
	DC_FAMILY_SUUNTO_VYPER,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	suunto_vyper_parser_get_datetime, /* datetime */
	suunto_vyper_parser_get_field, /* fields */
	suunto_vyper_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static unsigned int
suunto_vyper_parser_find_gasmix (suunto_vyper_parser_t *parser, unsigned int o2)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->oxygen[i])
			break;
		i++;
	}

	return i;
}

static dc_status_t
suunto_vyper_parser_cache (suunto_vyper_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	if (size < 18) {
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int ngasmixes = 1;
	unsigned int oxygen[NGASMIXES] = {0};
	if (data[6])
		oxygen[0] = data[6];
	else
		oxygen[0] = 21;

	// Parse the samples.
	unsigned int interval = data[3];
	unsigned int nsamples = 0;
	unsigned int depth = 0, maxdepth = 0;
	unsigned int offset = 14;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];
		if (value < 0x79 || value > 0x87) {
			// Delta depth.
			depth += (signed char) value;
			if (depth > maxdepth)
				maxdepth = depth;
			nsamples++;
		} else if (value == 0x87) {
			// Gas change event.
			if (offset + 1 > size) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}

			// Get the new gas mix.
			unsigned int o2 = data[offset++];

			// Find the gasmix in the list.
			unsigned int i = 0;
			while (i < ngasmixes) {
				if (o2 == oxygen[i])
					break;
				i++;
			}

			// Add it to list if not found.
			if (i >= ngasmixes) {
				if (i >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				oxygen[i] = o2;
				ngasmixes = i + 1;
			}
		}
	}

	// Check the end marker.
	unsigned int marker = offset;
	if (marker + 4 >= size || data[marker] != 0x80) {
		ERROR (abstract->context, "No valid end marker found!");
		return DC_STATUS_DATAFORMAT;
	}

	// Cache the data for later use.
	parser->divetime = nsamples * interval;
	parser->maxdepth = maxdepth;
	parser->marker = marker;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
	}
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_vyper_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	suunto_vyper_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (suunto_vyper_parser_t *) dc_parser_allocate (context, &suunto_vyper_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->marker = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 9 + 5)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + 9;

	if (datetime) {
		datetime->year   = p[0] + (p[0] < 90 ? 2000 : 1900);
		datetime->month  = p[1];
		datetime->day    = p[2];
		datetime->hour   = p[3];
		datetime->minute = p[4];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_vyper_parser_t *parser = (suunto_vyper_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	dc_gasmix_t *gas = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	// Cache the data.
	dc_status_t rc = suunto_vyper_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int gauge = data[4] & 0x40;
	unsigned int beginpressure = data[5] * 2;
	unsigned int endpressure   = data[parser->marker + 3] * 2;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			if (gauge)
				*((unsigned int *) value) = 0;
			else
				*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gas->usage = DC_USAGE_NONE;
			gas->helium = 0.0;
			gas->oxygen = parser->oxygen[flags] / 100.0;
			gas->nitrogen = 1.0 - gas->oxygen - gas->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			if (beginpressure == 0 && endpressure == 0)
				*((unsigned int *) value) = 0;
			else
				*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			if (gauge)
				tank->gasmix = DC_GASMIX_UNKNOWN;
			else
				tank->gasmix = 0;
			tank->beginpressure = beginpressure;
			tank->endpressure = endpressure;
			tank->usage = DC_USAGE_NONE;
			break;
		case DC_FIELD_TEMPERATURE_SURFACE:
			*((double *) value) = (signed char) data[8];
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed char) data[parser->marker + 1];
			break;
		case DC_FIELD_DIVEMODE:
			if (gauge) {
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
			} else {
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
			}
			break;
		case DC_FIELD_DECOMODEL:
			decomodel->type = DC_DECOMODEL_RGBM;
			decomodel->conservatism = (data[4] & 0x0F) / 3;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_vyper_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_vyper_parser_t *parser = (suunto_vyper_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	dc_sample_value_t sample = {0};

	// Cache the data.
	dc_status_t rc = suunto_vyper_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int gauge = data[4] & 0x40;

	// Time
	sample.time = 0;
	if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

	// Initial gas mix
	if (!gauge) {
		sample.gasmix = 0;
		if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
	}

	unsigned int depth = 0;
	unsigned int time = 0;
	unsigned int interval = data[3];
	unsigned int complete = 1;
	unsigned int offset = 14;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];

		if (complete) {
			// Time (seconds).
			time += interval;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
			complete = 0;
		}

		if (value < 0x79 || value > 0x87) {
			// Delta depth.
			depth += (signed char) value;

			// Depth (ft).
			sample.depth = depth * FEET;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			complete = 1;
		} else {
			// Event.
			unsigned int o2 = 0, idx = 0;
			sample.event.type = SAMPLE_EVENT_NONE;
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
				if (offset + 1 > size)
					return DC_STATUS_DATAFORMAT;

				o2 = data[offset++];
				idx = suunto_vyper_parser_find_gasmix (parser, o2);
				if (idx >= parser->ngasmixes) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}

				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				sample.event.type = SAMPLE_EVENT_NONE;
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

	// Time
	if (complete) {
		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
	}

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

	return DC_STATUS_SUCCESS;
}
