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

#include "oceanic_vtpro.h"
#include "oceanic_common.h"
#include "parser-private.h"
#include "array.h"
#include "units.h"
#include "utils.h"

typedef struct oceanic_vtpro_parser_t oceanic_vtpro_parser_t;

struct oceanic_vtpro_parser_t {
	parser_t base;
};

static parser_status_t oceanic_vtpro_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t oceanic_vtpro_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t oceanic_vtpro_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t oceanic_vtpro_parser_destroy (parser_t *abstract);

static const parser_backend_t oceanic_vtpro_parser_backend = {
	PARSER_TYPE_OCEANIC_VTPRO,
	oceanic_vtpro_parser_set_data, /* set_data */
	oceanic_vtpro_parser_get_datetime, /* datetime */
	oceanic_vtpro_parser_samples_foreach, /* samples_foreach */
	oceanic_vtpro_parser_destroy /* destroy */
};


static int
parser_is_oceanic_vtpro (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_vtpro_parser_backend;
}


parser_status_t
oceanic_vtpro_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	oceanic_vtpro_parser_t *parser = (oceanic_vtpro_parser_t *) malloc (sizeof (oceanic_vtpro_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &oceanic_vtpro_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_vtpro_parser_destroy (parser_t *abstract)
{
	if (! parser_is_oceanic_vtpro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_vtpro_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_oceanic_vtpro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_vtpro_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 8)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		// The logbook entry can only store the last digit of the year field,
		// but the full year is also available in the dive header.
		if (abstract->size < 40)
			datetime->year = bcd2dec (p[4] & 0x0F) + 2000;
		else
			datetime->year = bcd2dec (((p[32 + 3] & 0xC0) >> 2) + ((p[32 + 2] & 0xF0) >> 4)) + 2000;
		datetime->month  = (p[4] & 0xF0) >> 4;
		datetime->day    = bcd2dec (p[3]);
		datetime->hour   = bcd2dec (p[1] & 0x7F);
		datetime->minute = bcd2dec (p[0]);
		datetime->second = 0;

		// Convert to a 24-hour clock.
		datetime->hour %= 12;
		if (p[1] & 0x80)
			datetime->hour += 12;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_vtpro_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_oceanic_vtpro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;
	unsigned int interval = 0;
	switch ((data[0x27] >> 4) & 0x07) {
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
	default:
		interval = 0;
		break;
	}

	// Initialize the state for the timestamp processing.
	unsigned int timestamp = 0, count = 0, i = 0;

	unsigned int offset = 5 * PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		parser_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Get the current timestamp.
		unsigned int current = bcd2dec (data[offset + 1] & 0x0F) * 60 + bcd2dec (data[offset + 0]);
		if (current < timestamp) {
			WARNING ("Timestamp moved backwards.");
			return PARSER_STATUS_ERROR;
		}

		if (current != timestamp || count == 0) {
			// A sample with a new timestamp.
			i = 0;
			if (interval) {
				// With a time based sample interval, the maximum number
				// of samples for a single timestamp is always fixed.
				count = 60 / interval;
			} else {
				// With a depth based sample interval, the exact number
				// of samples for a single timestamp needs to be counted.
				count = 1;
				unsigned int idx = offset + PAGESIZE / 2 ;
				while (idx + PAGESIZE / 2 <= size - PAGESIZE) {
					// Ignore empty samples.
					if (array_isequal (data + idx, PAGESIZE / 2, 0x00)) {
						idx += PAGESIZE / 2;
						continue;
					}

					unsigned int next = bcd2dec (data[idx + 1] & 0x0F) * 60 + bcd2dec (data[idx + 0]);
					if (next != current)
						break;

					idx += PAGESIZE / 2;
					count++;
				}
			}
		} else {
			// A sample with the same timestamp.
			i++;
		}

		if (interval) {
			if (current > timestamp + 1) {
				WARNING ("Unexpected timestamp jump.");
				return PARSER_STATUS_ERROR;
			}
			if (i >= count) {
				WARNING ("Unexpected number of samples with the same timestamp.");
				return PARSER_STATUS_ERROR;
			}
		}

		// Store the current timestamp.
		timestamp = current;

		// Time.
		if (interval)
			time += interval;
		else
			time = timestamp * 60 + (i + 1) * 60.0 / count + 0.5;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_VTPRO;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);

		// Depth (ft)
		unsigned int depth = data[offset + 3];
		sample.depth = depth * FEET;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

		// Temperature (°F)
		unsigned int temperature = data[offset + 6];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

		offset += PAGESIZE / 2;
	}

	return PARSER_STATUS_SUCCESS;
}
