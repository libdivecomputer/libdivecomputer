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
#include <string.h>	// memcmp
#include <assert.h>

#include "suunto_d9.h"
#include "parser-private.h"
#include "utils.h"
#include "array.h"

#define SKIP 4

typedef struct suunto_d9_parser_t suunto_d9_parser_t;

struct suunto_d9_parser_t {
	parser_t base;
	unsigned int model;
};

static parser_status_t suunto_d9_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t suunto_d9_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t suunto_d9_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t suunto_d9_parser_destroy (parser_t *abstract);

static const parser_backend_t suunto_d9_parser_backend = {
	PARSER_TYPE_SUUNTO_D9,
	suunto_d9_parser_set_data, /* set_data */
	suunto_d9_parser_get_datetime, /* datetime */
	suunto_d9_parser_samples_foreach, /* samples_foreach */
	suunto_d9_parser_destroy /* destroy */
};


static int
parser_is_suunto_d9 (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_d9_parser_backend;
}


parser_status_t
suunto_d9_parser_create (parser_t **out, unsigned int model)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	suunto_d9_parser_t *parser = (suunto_d9_parser_t *) malloc (sizeof (suunto_d9_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &suunto_d9_parser_backend);

	// Set the default values.
	parser->model = model;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_d9_parser_destroy (parser_t *abstract)
{
	if (! parser_is_suunto_d9 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_d9_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_suunto_d9 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_d9_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t*) abstract;

	unsigned int offset = 0x15 - 4;
	if (parser->model == 0x15)
		offset += 6; // HelO2

	if (abstract->size < offset + 7)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data + offset;

	if (datetime) {
		datetime->hour   = p[0];
		datetime->minute = p[1];
		datetime->second = p[2];
		datetime->year   = p[3] + (p[4] << 8);
		datetime->month  = p[5];
		datetime->day    = p[6];
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
suunto_d9_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	suunto_d9_parser_t *parser = (suunto_d9_parser_t*) abstract;

	if (! parser_is_suunto_d9 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Offset to the configuration data.
	unsigned int config = 0x3E - SKIP;
	if (parser->model == 0x12)
		config += 1; // D4
	if (parser->model == 0x15)
		config += 74; // HelO2
	assert (config + 1 <= size);

	// Number of parameters in the configuration data.
	unsigned int nparams = data[config];

	// Offset to the profile data.
	unsigned int profile = config + 2 + nparams * 3;
	if (parser->model == 0x15)
		profile += 12; // HelO2
	assert (profile + 5 <= size);

	// Sample recording interval.
	unsigned int interval_sample_offset = 0x1C - SKIP;
	if (parser->model == 0x15)
		interval_sample_offset += 6; // HelO2
	unsigned int interval_sample = data[interval_sample_offset];
	assert (interval_sample > 0);

	// Temperature recording interval.
	unsigned int interval_temperature = data[config + 2 + (nparams - 1) * 3 + 1];
	assert (interval_temperature > 0);

	// Offset to the first marker position.
	unsigned int marker = array_uint16_le (data + profile + 3);

	unsigned int time = 0;
	unsigned int nsamples = 0;
	unsigned int offset = profile + 5;
	while (offset + 2 <= size) {
		parser_sample_value_t sample = {0};

		// Time (seconds).
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Depth (cm).
		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = depth / 100.0;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
		offset += 2;

		// Tank pressure (1/100 bar).
		if (nparams == 3) {
			assert (offset + 2 <= size);
			unsigned int pressure = array_uint16_le (data + offset);
			if (pressure != 0xFFFF) {
				sample.pressure.tank = 0;
				sample.pressure.value = pressure / 100.0;
				if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			}
			offset += 2;
		}

		// Temperature (degrees celcius).
		if (nsamples % interval_temperature == 0) {
			assert (offset + 1 <= size);
			sample.temperature = (signed char) data[offset];
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
			offset += 1;
		}

		// Events
		if ((nsamples + 1) == marker) {
			while (offset < size) {
				unsigned int event = data[offset++];
				unsigned int seconds, type, unknown, heading, percentage;
				unsigned int current, next;

				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				switch (event) {
				case 0x01: // Next Event Marker
					assert (offset + 4 <= size);
					current = array_uint16_le (data + offset + 0);
					next    = array_uint16_le (data + offset + 2);
					assert (marker == current);
					marker += next;
					offset += 4;
					break;
				case 0x02: // Surfaced
					assert (offset + 2 <= size);
					unknown = data[offset + 0];
					seconds = data[offset + 1];
					sample.event.type = SAMPLE_EVENT_SURFACE;
					sample.event.time = seconds;
					if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
					offset += 2;
					break;
				case 0x03: // Event
					assert (offset + 2 <= size);
					type    = data[offset + 0];
					seconds = data[offset + 1];
					switch (type & 0x7F) {
					case 0x00: // Voluntary Safety Stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_VOLUNTARY;
						break;
					case 0x01: // Mandatory Safety Stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_MANDATORY;
						break;
					case 0x02: // Deep Safety Stop
						sample.event.type = SAMPLE_EVENT_DEEPSTOP;
						break;
					case 0x03: // Deco
						sample.event.type = SAMPLE_EVENT_DECOSTOP;
						break;
					case 0x04: // Ascent Rate Warning
						sample.event.type = SAMPLE_EVENT_ASCENT;
						break;
					case 0x05: // Ceiling Broken
						sample.event.type = SAMPLE_EVENT_CEILING;
						break;
					case 0x06: // Mandatory Safety Stop Ceiling Error
						sample.event.type = SAMPLE_EVENT_CEILING_SAFETYSTOP;
						break;
					case 0x07: // Unknown (Deco related)
						sample.event.type = SAMPLE_EVENT_UNKNOWN;
						break;
					case 0x08: // Dive Time
						sample.event.type = SAMPLE_EVENT_DIVETIME;
						break;
					case 0x09: // Depth Alarm
						sample.event.type = SAMPLE_EVENT_MAXDEPTH;
						break;
					case 0x0A: // OLF 80
						sample.event.type = SAMPLE_EVENT_OLF;
						sample.event.value = 80;
						break;
					case 0x0B: // OLF 100
						sample.event.type = SAMPLE_EVENT_OLF;
						sample.event.value = 100;
						break;
					case 0x0C: // PO2
						sample.event.type = SAMPLE_EVENT_PO2;
						break;
					case 0x0D: // Air Time Warning
						sample.event.type = SAMPLE_EVENT_AIRTIME;
						break;
					case 0x0E: // RGBM Warning
						sample.event.type = SAMPLE_EVENT_RGBM;
						break;
					case 0x0F: // PO2 High
					case 0x10: // PO2 Low
						sample.event.type = SAMPLE_EVENT_PO2;
						break;
					case 0x11: // Tissue Level Warning
					case 0x12: // Tissue Calc Overflow
						sample.event.type = SAMPLE_EVENT_TISSUELEVEL;
						break;
					case 0x13: // Deep Safety Stop
						sample.event.type = SAMPLE_EVENT_DEEPSTOP;
						break;
					case 0x14: // Mandatory Safety Stop
						sample.event.type = SAMPLE_EVENT_SAFETYSTOP_MANDATORY;
						break;
					default: // Unknown
						WARNING ("Unknown event");
						break;
					}
					if (type & 0x80)
						sample.event.flags = SAMPLE_FLAGS_END;
					else
						sample.event.flags = SAMPLE_FLAGS_BEGIN;
					sample.event.time = seconds;
					if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
					offset += 2;
					break;
				case 0x04: // Bookmark/Heading
					assert (offset + 4 <= size);
					unknown = data[offset + 0];
					seconds = data[offset + 1];
					heading = array_uint16_le (data + offset + 2);
					if (heading == 0xFFFF) {
						sample.event.type = SAMPLE_EVENT_BOOKMARK;
						sample.event.value = 0;
					} else {
						sample.event.type = SAMPLE_EVENT_HEADING;
						sample.event.value = heading / 2;
					}
					sample.event.time = seconds;
					if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
					offset += 4;
					break;
				case 0x05: // Gas Change
					assert (offset + 2 <= size);
					percentage = data[offset + 0];
					seconds = data[offset + 1];
					sample.event.type = SAMPLE_EVENT_GASCHANGE;
					sample.event.time = seconds;
					sample.event.value = percentage;
					if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
					offset += 2;
					break;
				case 0x06: // Gas Change
					assert (offset + 4 <= size);
					unknown = data[offset + 0];
					unknown = data[offset + 1];
					percentage = data[offset + 2];
					seconds = data[offset + 3];
					sample.event.type = SAMPLE_EVENT_GASCHANGE;
					sample.event.time = seconds;
					sample.event.value = percentage;
					if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
					offset += 4;
					break;
				default:
					WARNING ("Unknown event");
					break;
				}

				if (event == 0x01)
					break;
			}
		}
		time += interval_sample;
		nsamples++;
	}

	return PARSER_STATUS_SUCCESS;
}
