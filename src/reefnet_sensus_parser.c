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

#include <stdlib.h>	// malloc, free

#include "reefnet_sensus.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

#define SAMPLE_DEPTH_ADJUST	13

typedef struct reefnet_sensus_parser_t reefnet_sensus_parser_t;

struct reefnet_sensus_parser_t {
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

static parser_status_t reefnet_sensus_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t reefnet_sensus_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t reefnet_sensus_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t reefnet_sensus_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t reefnet_sensus_parser_destroy (parser_t *abstract);

static const parser_backend_t reefnet_sensus_parser_backend = {
	PARSER_TYPE_REEFNET_SENSUS,
	reefnet_sensus_parser_set_data, /* set_data */
	reefnet_sensus_parser_get_datetime, /* datetime */
	reefnet_sensus_parser_get_field, /* fields */
	reefnet_sensus_parser_samples_foreach, /* samples_foreach */
	reefnet_sensus_parser_destroy /* destroy */
};


static int
parser_is_reefnet_sensus (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensus_parser_backend;
}


parser_status_t
reefnet_sensus_parser_create (parser_t **out, unsigned int devtime, dc_ticks_t systime)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) malloc (sizeof (reefnet_sensus_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &reefnet_sensus_parser_backend);

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
reefnet_sensus_parser_destroy (parser_t *abstract)
{
	if (! parser_is_reefnet_sensus (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensus_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t*) abstract;

	if (! parser_is_reefnet_sensus (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	return PARSER_STATUS_SUCCESS;
}


parser_status_t
reefnet_sensus_parser_set_calibration (parser_t *abstract, double atmospheric, double hydrostatic)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t*) abstract;

	if (! parser_is_reefnet_sensus (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensus_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	if (abstract->size < 2 + 4)
		return PARSER_STATUS_ERROR;

	unsigned int timestamp = array_uint32_le (abstract->data + 2);

	dc_ticks_t ticks = parser->systime - (parser->devtime - timestamp);

	if (!dc_datetime_localtime (datetime, ticks))
		return PARSER_STATUS_ERROR;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
reefnet_sensus_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	if (abstract->size < 7)
		return PARSER_STATUS_ERROR;

	if (!parser->cached) {
		const unsigned char *data = abstract->data;
		unsigned int size = abstract->size;

		unsigned int maxdepth = 0;
		unsigned int interval = data[1];
		unsigned int nsamples = 0, count = 0;

		unsigned int offset = 7;
		while (offset + 1 <= size) {
			// Depth.
			unsigned int depth = data[offset++];
			if (depth > maxdepth)
				maxdepth = depth;

			// Skip temperature byte.
			if ((nsamples % 6) == 0)
				offset++;

			// Current sample is complete.
			nsamples++;

			// The end of a dive is reached when 17 consecutive
			// depth samples of less than 3 feet have been found.
			if (depth < SAMPLE_DEPTH_ADJUST + 3) {
				count++;
				if (count == 17) {
					break;
				}
			} else {
				count = 0;
			}
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
			*((double *) value) = ((parser->maxdepth + 33.0 - (double) SAMPLE_DEPTH_ADJUST) * FSW - parser->atmospheric) / parser->hydrostatic;
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
reefnet_sensus_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t*) abstract;

	if (! parser_is_reefnet_sensus (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + 7 <= size) {
		if (data[offset] == 0xFF && data[offset + 6] == 0xFE) {
			
			unsigned int time = 0;
			unsigned int interval = data[offset + 1];	
			unsigned int nsamples = 0, count = 0;

			offset += 7;
			while (offset + 1 <= size) {
				parser_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time;
				if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

				// Depth (adjusted feet of seawater).
				unsigned int depth = data[offset++];
				sample.depth = ((depth + 33.0 - (double) SAMPLE_DEPTH_ADJUST) * FSW - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

				// Temperature (degrees Fahrenheit)
				if ((nsamples % 6) == 0) {
					if (offset + 1 > size)
						return PARSER_STATUS_ERROR;
					unsigned int temperature = data[offset++];
					sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
					if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
				}

				// Current sample is complete.
				nsamples++;

				// The end of a dive is reached when 17 consecutive  
				// depth samples of less than 3 feet have been found.
				if (depth < SAMPLE_DEPTH_ADJUST + 3) {
					count++;
					if (count == 17) {
						break;
					}
				} else {
					count = 0;
				}
			}
			break;
		} else {
			offset++;
		}
	}

	return PARSER_STATUS_SUCCESS;
}
