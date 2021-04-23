/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
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

#include "tecdiving_divecomputereu.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &tecdiving_divecomputereu_parser_vtable)

#define SZ_HEADER 100

typedef struct tecdiving_divecomputereu_parser_t tecdiving_divecomputereu_parser_t;

struct tecdiving_divecomputereu_parser_t {
	dc_parser_t base;
};

static dc_status_t tecdiving_divecomputereu_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t tecdiving_divecomputereu_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t tecdiving_divecomputereu_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t tecdiving_divecomputereu_parser_vtable = {
	sizeof(tecdiving_divecomputereu_parser_t),
	DC_FAMILY_TECDIVING_DIVECOMPUTEREU,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	tecdiving_divecomputereu_parser_get_datetime, /* datetime */
	tecdiving_divecomputereu_parser_get_field, /* fields */
	tecdiving_divecomputereu_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
tecdiving_divecomputereu_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	tecdiving_divecomputereu_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (tecdiving_divecomputereu_parser_t *) dc_parser_allocate (context, &tecdiving_divecomputereu_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
tecdiving_divecomputereu_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (datetime) {
		datetime->year   = data[2] + 2000;
		datetime->month  = data[3];
		datetime->day    = data[4];
		datetime->hour   = data[5];
		datetime->minute = data[6];
		datetime->second = data[7];
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
tecdiving_divecomputereu_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (data + 23) * 60;
			break;
		case DC_FIELD_AVGDEPTH:
			*((double *) value) = array_uint16_be (data + 27) / 100;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_be (data + 29) / 10;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_be (data + 14) / 1000.0;
			break;
		case DC_FIELD_TEMPERATURE_SURFACE:
			*((double *) value) = (signed char) data[17];
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed char) data[41];
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = (signed char) data[42];
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
tecdiving_divecomputereu_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = data[47];

	unsigned int offset = SZ_HEADER;
	while (offset + 8 <= size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_be (data + offset + 2);
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Temperature (Celsius).
		signed int temperature = (signed char) data[offset];
		sample.temperature = temperature;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

		// ppO2
		unsigned int ppo2 = data[offset + 1];
		sample.ppo2.sensor = DC_SENSOR_NONE;
		sample.ppo2.value = ppo2 / 10.0;
		if (callback) callback (DC_SAMPLE_PPO2, &sample, userdata);

		// Setpoint
		unsigned int setpoint = data[offset + 4];
		sample.setpoint = setpoint / 10.0;
		if (callback) callback (DC_SAMPLE_SETPOINT, &sample, userdata);

		offset += 8;
	}

	return DC_STATUS_SUCCESS;
}
