/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Jef Driesen
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

#include "sporasub_sp2.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &sporasub_sp2_parser_vtable)

#define SZ_HEADER 0x20
#define SZ_SAMPLE 0x04

typedef struct sporasub_sp2_parser_t sporasub_sp2_parser_t;

struct sporasub_sp2_parser_t {
	dc_parser_t base;
};

static dc_status_t sporasub_sp2_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t sporasub_sp2_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t sporasub_sp2_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t sporasub_sp2_parser_vtable = {
	sizeof(sporasub_sp2_parser_t),
	DC_FAMILY_SPORASUB_SP2,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	sporasub_sp2_parser_get_datetime, /* datetime */
	sporasub_sp2_parser_get_field, /* fields */
	sporasub_sp2_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
sporasub_sp2_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	sporasub_sp2_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (sporasub_sp2_parser_t *) dc_parser_allocate (context, &sporasub_sp2_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
sporasub_sp2_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (datetime) {
		datetime->year = data[4] + 2000;
		datetime->month = data[3];
		datetime->day = data[2];
		datetime->hour = data[7];
		datetime->minute = data[6];
		datetime->second = data[5];
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
sporasub_sp2_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	dc_salinity_t *water = (dc_salinity_t *) value;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int settings = data[0x1A];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = data[0x08] + data[0x09] * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 0x14) / 100.0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = array_uint16_le (data + 0x18) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = array_uint16_le (data + 0x16) / 10.0;
			break;
		case DC_FIELD_SALINITY:
			water->type = settings & 0x08 ? DC_WATER_FRESH : DC_WATER_SALT;
			water->density = 0.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
sporasub_sp2_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int nsamples = array_uint16_le(data);
	unsigned int settings = data[0x1A];

	// Get the sample interval.
	unsigned int interval_idx = settings & 0x03;
	const unsigned int intervals[] = {1, 2, 5, 10};
	if (interval_idx >= C_ARRAY_SIZE(intervals)) {
		ERROR (abstract->context, "Invalid sample interval index %u", interval_idx);
		return DC_STATUS_DATAFORMAT;
	}
	unsigned int interval = intervals[interval_idx];

	unsigned int time = 0;
	unsigned int count = 0;
	unsigned int offset = SZ_HEADER;
	while (offset + SZ_SAMPLE <= size && count < nsamples) {
		dc_sample_value_t sample = {0};

		unsigned int value = array_uint32_le (data + offset);
		unsigned int heartrate   = (value & 0xFF000000) >> 24;
		unsigned int temperature = (value & 0x00FFC000) >> 14;
		unsigned int depth =       (value & 0x00003FFF) >>  0;

		// Time (seconds)
		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth (1/100 m)
		sample.depth = depth / 100.0;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Temperature (1/10 °C)
		sample.temperature = temperature / 10.0 - 20.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

		// Heartrate
		if (heartrate) {
			sample.heartbeat = heartrate;
			if (callback) callback (DC_SAMPLE_HEARTBEAT, &sample, userdata);
		}

		offset += SZ_SAMPLE;
		count++;
	}

	return DC_STATUS_SUCCESS;
}
