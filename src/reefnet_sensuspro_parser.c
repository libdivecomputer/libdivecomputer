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

#include "reefnet_sensuspro.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &reefnet_sensuspro_parser_vtable)

typedef struct reefnet_sensuspro_parser_t reefnet_sensuspro_parser_t;

struct reefnet_sensuspro_parser_t {
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

static dc_status_t reefnet_sensuspro_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime);
static dc_status_t reefnet_sensuspro_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric);
static dc_status_t reefnet_sensuspro_parser_set_density (dc_parser_t *abstract, double density);
static dc_status_t reefnet_sensuspro_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t reefnet_sensuspro_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t reefnet_sensuspro_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t reefnet_sensuspro_parser_vtable = {
	sizeof(reefnet_sensuspro_parser_t),
	DC_FAMILY_REEFNET_SENSUSPRO,
	reefnet_sensuspro_parser_set_clock, /* set_clock */
	reefnet_sensuspro_parser_set_atmospheric, /* set_atmospheric */
	reefnet_sensuspro_parser_set_density, /* set_density */
	reefnet_sensuspro_parser_get_datetime, /* datetime */
	reefnet_sensuspro_parser_get_field, /* fields */
	reefnet_sensuspro_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
reefnet_sensuspro_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	reefnet_sensuspro_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (reefnet_sensuspro_parser_t *) dc_parser_allocate (context, &reefnet_sensuspro_parser_vtable, data, size);
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
reefnet_sensuspro_parser_set_clock (dc_parser_t *abstract, unsigned int devtime, dc_ticks_t systime)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	parser->devtime = devtime;
	parser->systime = systime;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_parser_set_atmospheric (dc_parser_t *abstract, double atmospheric)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	parser->atmospheric = atmospheric;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_parser_set_density (dc_parser_t *abstract, double density)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	parser->hydrostatic = density * GRAVITY;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	if (abstract->size < 6 + 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (abstract->data + 6);

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
reefnet_sensuspro_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t *) abstract;

	if (abstract->size < 12)
		return DC_STATUS_DATAFORMAT;

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
			unsigned int raw = array_uint16_le (data + offset);
			unsigned int depth = (raw & 0x01FF);
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
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (parser->maxdepth * FSW - parser->atmospheric) / parser->hydrostatic;
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
reefnet_sensuspro_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	reefnet_sensuspro_parser_t *parser = (reefnet_sensuspro_parser_t*) abstract;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[2] = {0xFF, 0xFF};

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + sizeof (header) <= size) {
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			if (offset + 10 > size)
				return DC_STATUS_DATAFORMAT;

			unsigned int time = 0;
			unsigned int interval = array_uint16_le (data + offset + 4);

			offset += 10;
			while (offset + sizeof (footer) <= size &&
				memcmp (data + offset, footer, sizeof (footer)) != 0)
			{
				unsigned int value = array_uint16_le (data + offset);
				unsigned int depth = (value & 0x01FF);
				unsigned int temperature = (value & 0xFE00) >> 9;

				dc_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time * 1000;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Temperature (°F)
				sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

				// Depth (absolute pressure in fsw)
				sample.depth = (depth * FSW - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

				offset += 2;
			}
			break;
		} else {
			offset++;
		}
	}

	return DC_STATUS_SUCCESS;
}
