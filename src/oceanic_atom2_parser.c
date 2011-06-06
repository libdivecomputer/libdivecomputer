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

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "parser-private.h"
#include "array.h"
#include "units.h"
#include "utils.h"

#define ATOM1       0x4250
#define EPIC        0x4257
#define VT3         0x4258
#define T3          0x4259
#define ATOM2       0x4342
#define GEO         0x4344
#define DATAMASK    0x4347
#define COMPUMASK   0x4348
#define OC1A        0x434E
#define VEO20       0x4359
#define VEO30       0x435A
#define ZENAIR      0x4442
#define GEO20       0x4446
#define VT4         0x4447
#define OC1B        0x4449
#define ATOM3       0x444C

typedef struct oceanic_atom2_parser_t oceanic_atom2_parser_t;

struct oceanic_atom2_parser_t {
	parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	double maxdepth;
};

static parser_status_t oceanic_atom2_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t oceanic_atom2_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t oceanic_atom2_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t oceanic_atom2_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t oceanic_atom2_parser_destroy (parser_t *abstract);

static const parser_backend_t oceanic_atom2_parser_backend = {
	PARSER_TYPE_OCEANIC_ATOM2,
	oceanic_atom2_parser_set_data, /* set_data */
	oceanic_atom2_parser_get_datetime, /* datetime */
	oceanic_atom2_parser_get_field, /* fields */
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
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

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
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	if (! parser_is_oceanic_atom2 (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

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
		case OC1A:
		case OC1B:
		case VT4:
		case ATOM3:
			datetime->year   = ((p[5] & 0xE0) >> 5) + ((p[7] & 0xE0) >> 2) + 2000;
			datetime->month  = (p[3] & 0x0F);
			datetime->day    = ((p[0] & 0x80) >> 3) + ((p[3] & 0xF0) >> 4);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0] & 0x7F);
			break;
		case VT3:
		case VEO20:
		case GEO20:
			datetime->year   = ((p[3] & 0xE0) >> 1) + (p[4] & 0x0F) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			datetime->day    = p[3] & 0x1F;
			datetime->hour   = bcd2dec (p[1] & 0x7F);
			datetime->minute = bcd2dec (p[0]);
			break;
		case ZENAIR:
			datetime->year   = (p[3] & 0x0F) + 2000;
			datetime->month  = (p[7] & 0xF0) >> 4;
			datetime->day    = ((p[3] & 0x80) >> 3) + ((p[5] & 0xF0) >> 4);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0]);
			break;
		default:
			datetime->year   = bcd2dec (((p[3] & 0xC0) >> 2) + (p[4] & 0x0F)) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			if (parser->model == T3)
				datetime->day = p[3] & 0x3F;
			else
				datetime->day = bcd2dec (p[3] & 0x3F);
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
oceanic_atom2_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int length = 11 * PAGESIZE / 2;
	unsigned int header = 4 * PAGESIZE;
	unsigned int footer = size - PAGESIZE;
	if (parser->model == GEO || parser->model == DATAMASK ||
		parser->model == GEO20 || parser->model == VEO20 ||
		parser->model == VEO30)
	{
		length -= PAGESIZE;
		header -= PAGESIZE;
	}

	if (size < length)
		return PARSER_STATUS_ERROR;

	if (!parser->cached) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		parser_status_t rc = oceanic_atom2_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (rc != PARSER_STATUS_SUCCESS)
			return rc;

		parser->cached = 1;
		parser->divetime = statistics.divetime;
		parser->maxdepth = statistics.maxdepth;
	}

	gasmix_t *gasmix = (gasmix_t *) value;

	unsigned int nitrox = 0;

	if (value) {
		switch (type) {
		case FIELD_TYPE_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case FIELD_TYPE_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + footer + 4) / 16.0 * FEET;
			break;
		case FIELD_TYPE_GASMIX_COUNT:
			if (parser->model == DATAMASK)
				*((unsigned int *) value) = 1;
			else
				*((unsigned int *) value) = 3;
			break;
		case FIELD_TYPE_GASMIX:
			if (parser->model == DATAMASK)
				nitrox = data[header + 3];
			else
				nitrox = data[header + 4 + flags];
			gasmix->helium = 0.0;
			gasmix->oxygen = (nitrox ? nitrox / 100.0 : 0.21);
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return PARSER_STATUS_UNSUPPORTED;
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
	if (parser->model == DATAMASK || parser->model == COMPUMASK ||
		parser->model == GEO || parser->model == GEO20 ||
		parser->model == VEO20 || parser->model == VEO30)
		header -= PAGESIZE;
	else if (parser->model == VT4)
		header += PAGESIZE;
	else if (parser->model == ATOM1)
		header -= 2 * PAGESIZE;

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

	unsigned int samplesize = PAGESIZE / 2;
	if (parser->model == OC1A || parser->model == OC1B)
		samplesize = PAGESIZE;

	int complete = 1;

	unsigned int tank = 0;
	unsigned int pressure = data[header + 2] + (data[header + 3] << 8);
	unsigned int temperature = data[header + 7];

	unsigned int offset = header + PAGESIZE / 2;
	while (offset + samplesize <= size - PAGESIZE) {
		parser_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, samplesize, 0x00) ||
			array_isequal (data + offset, samplesize, 0xFF)) {
			offset += samplesize;
			continue;
		}

		// Time.
		if (complete) {
			time += interval;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			complete = 0;
		}

		// The sample size is usually fixed, but some sample types have a
		// larger size. Check whether we have that many bytes available.
		unsigned int length = samplesize;
		if (data[offset + 0] == 0xBB) {
			length = PAGESIZE;
			if (offset + length > size - PAGESIZE)
				return PARSER_STATUS_ERROR;
		}

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_ATOM2;
		sample.vendor.size = length;
		sample.vendor.data = data + offset;
		if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);

		// Check for a tank switch sample.
		if (data[offset + 0] == 0xAA) {
			if (parser->model == DATAMASK || parser->model == COMPUMASK) {
				// Tank pressure (1 psi) and number
				tank = 0;
				pressure = (((data[offset + 7] << 8) + data[offset + 6]) & 0x0FFF);
			} else {
				// Tank pressure (2 psi) and number (one based index)
				tank = (data[offset + 1] & 0x03) - 1;
				if (parser->model == ATOM2 || parser->model == EPIC)
					pressure = (((data[offset + 3] << 8) + data[offset + 4]) & 0x0FFF) * 2;
				else
					pressure = (((data[offset + 4] << 8) + data[offset + 5]) & 0x0FFF) * 2;
			}
		} else if (data[offset + 0] == 0xBB) {
			// The surface time is not always a nice multiple of the samplerate.
			// The number of inserted surface samples is therefore rounded down
			// to keep the timestamps aligned at multiples of the samplerate.
			unsigned int surftime = 60 * bcd2dec (data[offset + 1]) + bcd2dec (data[offset + 2]);
			unsigned int nsamples = surftime / interval;

			for (unsigned int i = 0; i < nsamples; ++i) {
				if (complete) {
					time += interval;
					sample.time = time;
					if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
				}

				sample.depth = 0.0;
				if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
				complete = 1;
			}
		} else {
			// Temperature (°F)
			if (parser->model == GEO || parser->model == ATOM1) {
				temperature = data[offset + 6];
			} else if (parser->model == GEO20 || parser->model == VEO20 ||
				parser->model == VEO30 || parser->model == OC1A ||
				parser->model == OC1B) {
				temperature = data[offset + 3];
			} else if (parser->model == VT4 || parser->model == ATOM3) {
				temperature = ((data[offset + 7] & 0xF0) >> 4) | ((data[offset + 7] & 0x0C) << 2) | ((data[offset + 5] & 0x0C) << 4);
			} else {
				unsigned int sign;
				if (parser->model == ATOM2 || parser->model == EPIC)
					sign = (data[offset + 0] & 0x80) >> 7;
				else
					sign = (~data[offset + 0] & 0x80) >> 7;
				if (sign)
					temperature -= (data[offset + 7] & 0x0C) >> 2;
				else
					temperature += (data[offset + 7] & 0x0C) >> 2;
			}
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

			// Tank Pressure (psi)
			if (parser->model == OC1A || parser->model == OC1B)
				pressure = (data[offset + 10] + (data[offset + 11] << 8)) & 0x0FFF;
			else if (parser->model == ZENAIR || parser->model == VT4 || parser->model == ATOM3)
				pressure = (((data[offset + 0] & 0x03) << 8) + data[offset + 1]) * 5;
			else
				pressure -= data[offset + 1];
			sample.pressure.tank = tank;
			sample.pressure.value = pressure * PSI / BAR;
			if (callback && pressure != 10000) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);

			// Depth (1/16 ft)
			unsigned int depth;
			if (parser->model == GEO20 || parser->model == VEO20 ||
				parser->model == VEO30 || parser->model == OC1A ||
				parser->model == OC1B)
				depth = (data[offset + 4] + (data[offset + 5] << 8)) & 0x0FFF;
			else if (parser->model == ATOM1)
				depth = data[offset + 3] * 16;
			else
				depth = (data[offset + 2] + (data[offset + 3] << 8)) & 0x0FFF;
			sample.depth = depth / 16.0 * FEET;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			complete = 1;
		}

		offset += length;
	}

	return PARSER_STATUS_SUCCESS;
}
