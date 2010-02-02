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

#include "reefnet_sensusultra.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

typedef struct reefnet_sensusultra_parser_t reefnet_sensusultra_parser_t;

struct reefnet_sensusultra_parser_t {
	parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
	// Clock synchronization.
	unsigned int devtime;
	dc_ticks_t systime;
};

static parser_status_t reefnet_sensusultra_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t reefnet_sensusultra_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t reefnet_sensusultra_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t reefnet_sensusultra_parser_destroy (parser_t *abstract);

static const parser_backend_t reefnet_sensusultra_parser_backend = {
	PARSER_TYPE_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_parser_set_data, /* set_data */
	reefnet_sensusultra_parser_get_datetime, /* datetime */
	reefnet_sensusultra_parser_samples_foreach, /* samples_foreach */
	reefnet_sensusultra_parser_destroy /* destroy */
};


static int
parser_is_reefnet_sensusultra (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensusultra_parser_backend;
}


parser_status_t
reefnet_sensusultra_parser_create (parser_t **out, unsigned int devtime, dc_ticks_t systime)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) malloc (sizeof (reefnet_sensusultra_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &reefnet_sensusultra_parser_backend);

	// Set the default values.
	parser->atmospheric = ATM;
	parser->hydrostatic = 1025.0 * GRAVITY;
	parser->devtime = devtime;
	parser->systime = systime;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensusultra_parser_destroy (parser_t *abstract)
{
	if (! parser_is_reefnet_sensusultra (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensusultra_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_reefnet_sensusultra (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


parser_status_t
reefnet_sensusultra_parser_set_calibration (parser_t *abstract, double atmospheric, double hydrostatic)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	if (! parser_is_reefnet_sensusultra (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensusultra_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	if (abstract->size < 4 + 4)
		return PARSER_STATUS_ERROR;

	unsigned int timestamp = array_uint32_le (abstract->data + 4);

	dc_ticks_t ticks = parser->systime - (parser->devtime - timestamp);

	if (!dc_datetime_localtime (datetime, ticks))
		return PARSER_STATUS_ERROR;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensusultra_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	if (! parser_is_reefnet_sensusultra (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + sizeof (header) <= size) {
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			assert (offset + 16 <= size);

			unsigned int time = 0;
			unsigned int interval = array_uint16_le (data + offset + 8);

			offset += 16;
			while (offset + sizeof (footer) <= size && 
				memcmp (data + offset, footer, sizeof (footer)) != 0) 
			{
				parser_sample_value_t sample = {0};

				// Time (seconds)
				sample.time = time;
				if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

				// Temperature (0.01 °K)
				unsigned int temperature = array_uint16_le (data + offset);
				sample.temperature = temperature / 100.0 - 273.15;
				if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

				// Depth (absolute pressure in millibar)
				unsigned int depth = array_uint16_le (data + offset + 2);
				sample.depth = (depth * BAR / 1000.0 - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

				time += interval;
				offset += 4;
			}
			break;
		} else {
			offset++;
		}
	}

	return PARSER_STATUS_SUCCESS;
}
