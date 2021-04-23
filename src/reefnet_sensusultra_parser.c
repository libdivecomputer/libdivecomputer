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

#include <libdivecomputer/units.h>

#include "reefnet_sensusultra.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &reefnet_sensusultra_parser_vtable)

typedef struct reefnet_sensusultra_parser_t reefnet_sensusultra_parser_t;

struct reefnet_sensusultra_parser_t {
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

static dc_status_t reefnet_sensusultra_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime);
static dc_status_t reefnet_sensusultra_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric);
static dc_status_t reefnet_sensusultra_parser_set_density (dc_parser_t *abstract, double density);
static dc_status_t reefnet_sensusultra_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t reefnet_sensusultra_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t reefnet_sensusultra_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t reefnet_sensusultra_parser_vtable = {
	sizeof(reefnet_sensusultra_parser_t),
	DC_FAMILY_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_parser_set_clock, /* set_clock */
	reefnet_sensusultra_parser_set_atmospheric, /* set_atmospheric */
	reefnet_sensusultra_parser_set_density, /* set_density */
	reefnet_sensusultra_parser_get_datetime, /* datetime */
	reefnet_sensusultra_parser_get_field, /* fields */
	reefnet_sensusultra_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
reefnet_sensusultra_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	reefnet_sensusultra_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (reefnet_sensusultra_parser_t *) dc_parser_allocate (context, &reefnet_sensusultra_parser_vtable, data, size);
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
reefnet_sensusultra_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	parser->devtime = devtime;
	parser->systime = systime;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	parser->atmospheric = atmospheric;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_set_density (dc_parser_t *abstract, double density)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	parser->hydrostatic = density * GRAVITY;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	if (abstract->size < 4 + 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (abstract->data + 4);

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
reefnet_sensusultra_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	if (abstract->size < 20)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

		const unsigned char *data = abstract->data;
		unsigned int size = abstract->size;

		unsigned int interval = array_uint16_le (data + 8);
		unsigned int threshold = array_uint16_le (data + 10);

		unsigned int maxdepth = 0;
		unsigned int nsamples = 0;
		unsigned int offset = 16;
		while (offset + sizeof (footer) <= size &&
			memcmp (data + offset, footer, sizeof (footer)) != 0)
		{
			unsigned int depth = array_uint16_le (data + offset + 2);
			if (depth >= threshold) {
				if (depth > maxdepth)
					maxdepth = depth;
				nsamples++;
			}

			offset += 4;
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
			*((double *) value) = (parser->maxdepth * BAR / 1000.0 - parser->atmospheric) / parser->hydrostatic;
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
reefnet_sensusultra_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + sizeof (header) <= size) {
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			if (offset + 16 > size)
				return DC_STATUS_DATAFORMAT;

			unsigned int time = 0;
			unsigned int interval = array_uint16_le (data + offset + 8);

			offset += 16;
			while (offset + sizeof (footer) <= size &&
				memcmp (data + offset, footer, sizeof (footer)) != 0)
			{
				dc_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time * 1000;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Temperature (0.01 °K)
				unsigned int temperature = array_uint16_le (data + offset);
				sample.temperature = temperature / 100.0 - 273.15;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

				// Depth (absolute pressure in millibar)
				unsigned int depth = array_uint16_le (data + offset + 2);
				sample.depth = (depth * BAR / 1000.0 - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

				offset += 4;
			}
			break;
		} else {
			offset++;
		}
	}

	return DC_STATUS_SUCCESS;
}
