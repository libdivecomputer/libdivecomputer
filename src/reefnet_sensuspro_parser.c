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

#include "reefnet_sensuspro.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

typedef struct reefnet_sensuspro_parser_t reefnet_sensuspro_parser_t;

struct reefnet_sensuspro_parser_t {
	parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
	// Clock synchronization.
	unsigned int devtime;
	dc_ticks_t systime;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
};

static parser_status_t reefnet_sensuspro_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t reefnet_sensuspro_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t reefnet_sensuspro_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t reefnet_sensuspro_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t reefnet_sensuspro_parser_destroy (parser_t *abstract);

static const parser_backend_t reefnet_sensuspro_parser_backend = {
	PARSER_TYPE_REEFNET_SENSUSPRO,
	reefnet_sensuspro_parser_set_data, /* set_data */
	reefnet_sensuspro_parser_get_datetime, /* datetime */
	reefnet_sensuspro_parser_get_field, /* fields */
	reefnet_sensuspro_parser_samples_foreach, /* samples_foreach */
	reefnet_sensuspro_parser_destroy /* destroy */
};


static int
parser_is_reefnet_sensuspro (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensuspro_parser_backend;
}


parser_status_t
reefnet_sensuspro_parser_create (parser_t **out, unsigned int devtime, dc_ticks_t systime)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) malloc (sizeof (reefnet_sensuspro_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &reefnet_sensuspro_parser_backend);

	// Set the default values.
	parser->atmospheric = ATM;
	parser->hydrostatic = 1025.0 * GRAVITY;
	parser->devtime = devtime;
	parser->systime = systime;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensuspro_parser_destroy (parser_t *abstract)
{
	if (! parser_is_reefnet_sensuspro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensuspro_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t*) abstract;

	if (! parser_is_reefnet_sensuspro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	return PARSER_STATUS_SUCCESS;
}


parser_status_t
reefnet_sensuspro_parser_set_calibration (parser_t *abstract, double atmospheric, double hydrostatic)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t*) abstract;

	if (! parser_is_reefnet_sensuspro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensuspro_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	if (abstract->size < 6 + 4)
		return PARSER_STATUS_ERROR;

	unsigned int timestamp = array_uint32_le (abstract->data + 6);

	dc_ticks_t ticks = parser->systime - (parser->devtime - timestamp);

	if (!dc_datetime_localtime (datetime, ticks))
		return PARSER_STATUS_ERROR;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensuspro_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	if (abstract->size < 12)
		return PARSER_STATUS_ERROR;

	if (!parser->cached) {
		const unsigned char footer[2] = {0xFF, 0xFF};

		const unsigned char *data = abstract->data;
		unsigned int size = abstract->size;

		unsigned int interval = array_uint16_le (data + 4);

		unsigned int maxdepth = 0;
		unsigned int nsamples = 0;
		unsigned int offset = 10;
		while (offset + sizeof (footer) <= size &&
			memcmp (data + offset, footer, sizeof (footer)) != 0)
		{
			unsigned int value = array_uint16_le (data + offset);
			unsigned int depth = (value & 0x01FF);
			if (depth > maxdepth)
				maxdepth = depth;

			nsamples++;

			offset += 2;
		}

		parser->cached = 1;
		parser->divetime = nsamples * interval;
		parser->maxdepth = maxdepth;
	}

	if (value) {
		switch (type) {
		case FIELD_TYPE_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case FIELD_TYPE_MAXDEPTH:
			*((double *) value) = (parser->maxdepth * FSW - parser->atmospheric) / parser->hydrostatic;
			break;
		case FIELD_TYPE_GASMIX_COUNT:
			*((unsigned int *) value) = 0;
			break;
		default:
			return PARSER_STATUS_UNSUPPORTED;
		}
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensuspro_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t*) abstract;

	if (! parser_is_reefnet_sensuspro (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[2] = {0xFF, 0xFF};

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + sizeof (header) <= size) {
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			if (offset + 10 > size)
				return PARSER_STATUS_ERROR;

			unsigned int time = 0;
			unsigned int interval = array_uint16_le (data + offset + 4);	

			offset += 10;
			while (offset + sizeof (footer) <= size && 
				memcmp (data + offset, footer, sizeof (footer)) != 0) 
			{
				unsigned int value = array_uint16_le (data + offset);
				unsigned int depth = (value & 0x01FF);
				unsigned int temperature = (value & 0xFE00) >> 9;

				parser_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time;
				if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

				// Temperature (°F)
				sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
				if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

				// Depth (absolute pressure in fsw)
				sample.depth = (depth * FSW - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

				offset += 2;
			}
			break;
		} else {
			offset++;
		}
	}

	return PARSER_STATUS_SUCCESS;
}
