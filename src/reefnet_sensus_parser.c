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

#include <libdivecomputer/units.h>

#include "reefnet_sensus.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &reefnet_sensus_parser_vtable)

#define SAMPLE_DEPTH_ADJUST	13

typedef struct reefnet_sensus_parser_t reefnet_sensus_parser_t;

struct reefnet_sensus_parser_t {
	dc_parser_t base;
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

static dc_status_t reefnet_sensus_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime);
static dc_status_t reefnet_sensus_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric);
static dc_status_t reefnet_sensus_parser_set_density (dc_parser_t *abstract, double density);
static dc_status_t reefnet_sensus_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t reefnet_sensus_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t reefnet_sensus_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t reefnet_sensus_parser_vtable = {
	sizeof(reefnet_sensus_parser_t),
	DC_FAMILY_REEFNET_SENSUS,
	reefnet_sensus_parser_set_clock, /* set_clock */
	reefnet_sensus_parser_set_atmospheric, /* set_atmospheric */
	reefnet_sensus_parser_set_density, /* set_density */
	reefnet_sensus_parser_get_datetime, /* datetime */
	reefnet_sensus_parser_get_field, /* fields */
	reefnet_sensus_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
reefnet_sensus_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	reefnet_sensus_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (reefnet_sensus_parser_t *) dc_parser_allocate (context, &reefnet_sensus_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->atmospheric = DEF_ATMOSPHERIC;
	parser->hydrostatic = DEF_DENSITY_SALT * GRAVITY;
	parser->devtime = 0;
	parser->systime = 0;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	parser->devtime = devtime;
	parser->systime = systime;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	parser->atmospheric = atmospheric;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_set_density (dc_parser_t *abstract, double density)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	parser->hydrostatic = density * GRAVITY;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	if (abstract->size < 2 + 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (abstract->data + 2);

	dc_ticks_t ticks = parser->systime;
	if (timestamp < parser->devtime) {
		ticks -= parser->devtime - timestamp;
	} else {
		ticks += timestamp - parser->devtime;
	}

	if (!dc_datetime_localtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t *) abstract;

	if (abstract->size < 7)
		return DC_STATUS_DATAFORMAT;

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
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = ((parser->maxdepth + 33.0 - (double) SAMPLE_DEPTH_ADJUST) * FSW - parser->atmospheric) / parser->hydrostatic;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensus_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	reefnet_sensus_parser_t *parser = (reefnet_sensus_parser_t*) abstract;

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
				dc_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time * 1000;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Depth (adjusted feet of seawater).
				unsigned int depth = data[offset++];
				sample.depth = ((depth + 33.0 - (double) SAMPLE_DEPTH_ADJUST) * FSW - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

				// Temperature (degrees Fahrenheit)
				if ((nsamples % 6) == 0) {
					if (offset + 1 > size)
						return DC_STATUS_DATAFORMAT;
					unsigned int temperature = data[offset++];
					sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
					if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
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

	return DC_STATUS_SUCCESS;
}
