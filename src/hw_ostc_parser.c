/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#include "hw_ostc.h"
#include "parser-private.h"
#include "array.h"
#include "utils.h"

#define NINFO 6

typedef struct hw_ostc_parser_t hw_ostc_parser_t;

struct hw_ostc_parser_t {
	parser_t base;
};

typedef struct hw_ostc_sample_info_t {
	unsigned int divisor;
	unsigned int size;
} hw_ostc_sample_info_t;

static parser_status_t hw_ostc_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t hw_ostc_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t hw_ostc_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t hw_ostc_parser_destroy (parser_t *abstract);

static const parser_backend_t hw_ostc_parser_backend = {
	PARSER_TYPE_HW_OSTC,
	hw_ostc_parser_set_data, /* set_data */
	hw_ostc_parser_get_datetime, /* datetime */
	hw_ostc_parser_samples_foreach, /* samples_foreach */
	hw_ostc_parser_destroy /* destroy */
};


static int
parser_is_hw_ostc (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &hw_ostc_parser_backend;
}


parser_status_t
hw_ostc_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) malloc (sizeof (hw_ostc_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &hw_ostc_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
hw_ostc_parser_destroy (parser_t *abstract)
{
	if (! parser_is_hw_ostc (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
hw_ostc_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_hw_ostc (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
hw_ostc_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 8)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = p[5] + 2000;
		datetime->month  = p[3];
		datetime->day    = p[4];
		datetime->hour   = p[6];
		datetime->minute = p[7];
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
hw_ostc_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_hw_ostc (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Check the profile version
	if (size < 47 || data[2] != 0x20)
		return PARSER_STATUS_ERROR;

	// Get the sample rate.
	unsigned int samplerate = data[36];

	// Get the extended sample configuration.
	hw_ostc_sample_info_t info[NINFO];
	for (unsigned int i = 0; i < NINFO; ++i) {
		info[i].divisor = (data[37 + i] & 0x0F);
		info[i].size    = (data[37 + i] & 0xF0) >> 4;
	}

	unsigned int time = 0;
	unsigned int nsamples = 0;

	unsigned int offset = 47;
	while (offset + 3 <= size) {
		parser_sample_value_t sample = {0};

		nsamples++;

		// Time (seconds).
		time += samplerate;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Depth (mbar).
		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = depth / 100.0;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
		offset += 2;

		// Extended sample info.
		unsigned int length =  data[offset] & 0x7F;
		unsigned int events = (data[offset] & 0x80) >> 7;
		offset += 1;

		// Check for buffer overflows.
		if (offset + length > size)
			return PARSER_STATUS_ERROR;

		// Events.
		if (events) {
			// Get the event byte.
			unsigned int value = data[offset++];

			// Alarms
			switch (value & 0x0F) {
			case 0: // No Alarm
			case 1: // Slow
			case 2: // Deco Stop missed
			case 3: // Deep Stop missed
			case 4: // ppO2 Low Warning
			case 5: // ppO2 High Warning
			case 6: // Manual Marker
			case 7: // Low Battery
				break;
			}

			// Manual Gas Set
			if (value & 0x10) {
				offset += 2;
			}

			// Gas Change
			if (value & 0x20) {
				offset++;
			}
		}

		// Extended sample info.
		for (unsigned int i = 0; i < NINFO; ++i) {
			if (info[i].divisor && (nsamples % info[i].divisor) == 0) {
				unsigned int value = 0;
				switch (i) {
				case 0: // Temperature (0.1 °C).
					assert (info[i].size == 2);
					value = array_uint16_le (data + offset);
					sample.temperature = value / 10.0;
					if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
					break;
				case 1: // Deco/NDL Status
					break;
				case 2: // Tank pressure
					assert (info[i].size == 2);
					value = array_uint16_le (data + offset);
					sample.pressure.tank = 0;
					sample.pressure.value = value;
					if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
					break;
				case 3: // ppO2 Sensor Values
					break;
				default: // Not yet used.
					break;
				}

				offset += info[i].size;
			}
		}
	}

	assert (data[offset] == 0xFD && data[offset + 1] == 0xFD);

	return PARSER_STATUS_SUCCESS;
}
