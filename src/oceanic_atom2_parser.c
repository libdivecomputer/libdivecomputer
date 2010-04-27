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

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "parser-private.h"
#include "array.h"
#include "units.h"
#include "utils.h"

typedef struct oceanic_atom2_parser_t oceanic_atom2_parser_t;

struct oceanic_atom2_parser_t {
	parser_t base;
	unsigned int model;
};

static parser_status_t oceanic_atom2_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t oceanic_atom2_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t oceanic_atom2_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t oceanic_atom2_parser_destroy (parser_t *abstract);

static const parser_backend_t oceanic_atom2_parser_backend = {
	PARSER_TYPE_OCEANIC_ATOM2,
	oceanic_atom2_parser_set_data, /* set_data */
	oceanic_atom2_parser_get_datetime, /* datetime */
	oceanic_atom2_parser_samples_foreach, /* samples_foreach */
	oceanic_atom2_parser_destroy /* destroy */
};


static int
parser_is_oceanic_atom2 (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_atom2_parser_backend;
}


parser_status_t
oceanic_atom2_parser_create (parser_t **out, unsigned int model)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) malloc (sizeof (oceanic_atom2_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &oceanic_atom2_parser_backend);

	// Set the default values.
	parser->model = model;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_destroy (parser_t *abstract)
{
	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	if (abstract->size < 8)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		switch (parser->model) {
		case 0x434E: // OC1
			datetime->year   = ((p[5] & 0xE0) >> 5) + ((p[7] & 0xE0) >> 2) + 2000;
			datetime->month  = (p[3] & 0x0F);
			datetime->day    = ((p[0] & 0x80) >> 3) + ((p[3] & 0xF0) >> 4);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0] & 0x7F);
			break;
		case 0x4258: // VT3
			datetime->year   = ((p[3] & 0xE0) >> 1) + (p[4] & 0x0F) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			datetime->day    = p[3] & 0x1F;
			datetime->hour   = bcd2dec (p[1] & 0x7F);
			datetime->minute = bcd2dec (p[0]);
			break;
		default: // Atom 2
			datetime->year   = bcd2dec (((p[3] & 0xC0) >> 2) + (p[4] & 0x0F)) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			datetime->day    = bcd2dec (p[3] & 0x3F);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0]);
			break;
		}
		datetime->second = 0;

		// Convert to a 24-hour clock.
		datetime->hour %= 12;
		if (p[1] & 0x80)
			datetime->hour += 12;

		/*
		 * Workaround for the year 2010 problem.
		 *
		 * In theory there are more than enough bits available to store years
		 * past 2010. Unfortunately some models do not use all those bits and
		 * store only the last digit of the year. We try to guess the missing
		 * information based on the current year. This should work in most
		 * cases, except when the dive is more than 10 years old or in the
		 * future (due to an incorrect clock on the device or the host system).
		 *
		 * Note that we are careful not to apply any guessing when the year is
		 * actually stored with more bits. We don't want the code to break when
		 * a firmware update fixes this bug.
		 */

		if (datetime->year < 2010) {
			// Retrieve the current year.
			dc_datetime_t now = {0};
			if (dc_datetime_localtime (&now, dc_datetime_now ()) &&
				now.year >= 2010)
			{
				// Guess the correct decade.
				int decade = (now.year / 10) * 10;
				if (datetime->year % 10 > now.year % 10)
					decade -= 10; /* Force back to the previous decade. */

				// Adjust the year.
				datetime->year += decade - 2000;
			}
		}
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
oceanic_atom2_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int header = 4 * PAGESIZE;
	if (parser->model == 0x4344 || parser->model == 0x4347)
		header -= PAGESIZE;

	if (size < header + 3 * PAGESIZE / 2)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;
	unsigned interval = 0;
	switch (data[0x17] & 0x03) {
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
	}

	int complete = 1;

	unsigned int tank = 0;
	unsigned int pressure = data[header + 2] + (data[header + 3] << 8);
	unsigned int temperature = data[header + 7];

	unsigned int offset = header + PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		parser_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Time.
		if (complete) {
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
		}

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_ATOM2;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);

		// Check for a tank switch sample.
		if (data[offset + 0] == 0xAA) {
			if (parser->model == 0x4347) {
				// Tank pressure (1 psi) and number
				tank = 0;
				pressure = (((data[offset + 7] << 8) + data[offset + 6]) & 0x0FFF);
			} else {
				// Tank pressure (2 psi) and number (one based index)
				tank = (data[offset + 1] & 0x03) - 1;
				pressure = (((data[offset + 4] << 8) + data[offset + 5]) & 0x0FFF) * 2;
			}

			complete = 0;
		} else {
			// Temperature (°F)
			if (parser->model == 0x4344) {
				temperature = data[offset + 6];
			} else {
				if (data[offset + 0] & 0x80)
					temperature += (data[offset + 7] & 0xFC) >> 2;
				else
					temperature -= (data[offset + 7] & 0xFC) >> 2;
			}
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

			// Tank Pressure (psi)
			pressure -= data[offset + 1];
			sample.pressure.tank = tank;
			sample.pressure.value = pressure * PSI / BAR;
			if (callback && pressure != 10000) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

			// Depth (1/16 ft)
			unsigned int depth = (data[offset + 2] + (data[offset + 3] << 8)) & 0x0FFF;
			sample.depth = depth / 16.0 * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			complete = 1;
		}

		offset += PAGESIZE / 2;
	}

	return PARSER_STATUS_SUCCESS;
}
