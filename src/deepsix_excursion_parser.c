/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Ryan Gardner, Jef Driesen
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <libdivecomputer/units.h>

#include "deepsix_excursion.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define HEADERSIZE 156

#define ALARM        0x0001
#define TEMPERATURE  0x0002
#define DECO         0x0003
#define CEILING      0x0004
#define CNS          0x0005

#define DENSITY 1024.0

typedef struct deepsix_excursion_parser_t {
	dc_parser_t base;
} deepsix_excursion_parser_t;

static dc_status_t deepsix_excursion_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t deepsix_excursion_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t deepsix_excursion_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t deepsix_excursion_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t deepsix_parser_vtable = {
	sizeof(deepsix_excursion_parser_t),
	DC_FAMILY_DEEPSIX_EXCURSION,
	deepsix_excursion_parser_set_data, /* set_data */
	deepsix_excursion_parser_get_datetime, /* datetime */
	deepsix_excursion_parser_get_field, /* fields */
	deepsix_excursion_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
deepsix_excursion_parser_create (dc_parser_t **out, dc_context_t *context)
{
	deepsix_excursion_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (deepsix_excursion_parser_t *) dc_parser_allocate (context, &deepsix_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADERSIZE)
		return DC_STATUS_DATAFORMAT;

	if (datetime) {
		datetime->year = data[12] + 2000;
		datetime->month = data[13];
		datetime->day = data[14];
		datetime->hour = data[15];
		datetime->minute = data[16];
		datetime->second = data[17];
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADERSIZE)
		return DC_STATUS_DATAFORMAT;

	unsigned int atmospheric = array_uint32_le(data + 56);

	dc_salinity_t *water = (dc_salinity_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint32_le(data + 20);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (signed int)(array_uint32_le(data + 28) - atmospheric) * (BAR / 1000.0) / (DENSITY * GRAVITY);
			break;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed int) array_uint32_le(data + 32) / 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = atmospheric / 1000.0;
			break;
		case DC_FIELD_SALINITY:
			water->type = DC_WATER_SALT;
			water->density = DENSITY;
			break;
		case DC_FIELD_DIVEMODE:
			switch (array_uint32_le(data + 4)) {
			case 0:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case 1:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case 2:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADERSIZE)
		return DC_STATUS_DATAFORMAT;

	int firmware4c = memcmp(data + 48, "D01-4C", 6) == 0;

	unsigned int maxtype = firmware4c ? TEMPERATURE : CNS;

	unsigned int interval = array_uint32_le(data + 24);
	unsigned int atmospheric = array_uint32_le(data + 56);

	unsigned int time = 0;
	unsigned int offset = HEADERSIZE;
	while (offset + 1 < size) {
		dc_sample_value_t sample = {0};

		// Get the sample type.
		unsigned int type = data[offset];
		if (type < 1 || type > maxtype) {
			ERROR (abstract->context, "Unknown sample type (%u).", type);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the sample length.
		unsigned int length = 1;
		if (type == ALARM || type == CEILING) {
			length = 8;
		} else if (type == TEMPERATURE || type == DECO || type == CNS) {
			length = 6;
		}

		// Verify the length.
		if (offset + length > size) {
			WARNING (abstract->context, "Unexpected end of data.");
			break;
		}

		unsigned int misc = data[offset + 1];
		unsigned int depth = array_uint16_le(data + offset + 2);

		if (type == TEMPERATURE) {
			time += interval;
			sample.time = time;
			if (callback) callback(DC_SAMPLE_TIME, sample, userdata);

			sample.depth = (signed int)(depth - atmospheric) * (BAR / 1000.0) / (DENSITY * GRAVITY);
			if (callback) callback(DC_SAMPLE_DEPTH, sample, userdata);
		}

		if (type == ALARM) {
			unsigned int alarm_time  = array_uint16_le(data + offset + 4);
			unsigned int alarm_value = array_uint16_le(data + offset + 6);
		} else if (type == TEMPERATURE) {
			unsigned int temperature = array_uint16_le(data + offset + 4);
			if (firmware4c) {
				if (temperature > 1300) {
					length = 8;
				} else if (temperature >= 10) {
					sample.temperature = temperature / 10.0;
					if (callback) callback(DC_SAMPLE_TEMPERATURE, sample, userdata);
				}
			} else {
				sample.temperature = temperature / 10.0;
				if (callback) callback(DC_SAMPLE_TEMPERATURE, sample, userdata);
			}
		} else if (type == DECO) {
			unsigned int deco = array_uint16_le(data + offset + 4);
		} else if (type == CEILING) {
			unsigned int ceiling_depth = array_uint16_le(data + offset + 4);
			unsigned int ceiling_time  = array_uint16_le(data + offset + 6);
		} else if (type == CNS) {
			unsigned int cns = array_uint16_le(data + offset + 4);
			sample.cns = cns;
			if (callback) callback(DC_SAMPLE_CNS, sample, userdata);
		}

		offset += length;
	}

	return DC_STATUS_SUCCESS;
}
