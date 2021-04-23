/*
 * libdivecomputer
 *
 * Copyright (C) 2019 Linus Torvalds
 * Copyright (C) 2022 Jef Driesen
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

#include <libdivecomputer/units.h>

#include "deepblu_cosmiq.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define SCUBA    2
#define GAUGE    3
#define FREEDIVE 4

#define SZ_HEADER 36
#define SZ_SAMPLE 4

typedef struct deepblu_cosmiq_parser_t {
	dc_parser_t base;
	double hydrostatic;
} deepblu_cosmiq_parser_t;

static dc_status_t deepblu_cosmiq_parser_set_density (dc_parser_t *abstract, double density);
static dc_status_t deepblu_cosmiq_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t deepblu_cosmiq_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t deepblu_cosmiq_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t deepblu_cosmiq_parser_vtable = {
	sizeof(deepblu_cosmiq_parser_t),
	DC_FAMILY_DEEPBLU_COSMIQ,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	deepblu_cosmiq_parser_set_density, /* set_density */
	deepblu_cosmiq_parser_get_datetime, /* datetime */
	deepblu_cosmiq_parser_get_field, /* fields */
	deepblu_cosmiq_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
deepblu_cosmiq_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	deepblu_cosmiq_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (deepblu_cosmiq_parser_t *) dc_parser_allocate (context, &deepblu_cosmiq_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->hydrostatic = DEF_DENSITY_SALT * GRAVITY;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_parser_set_density (dc_parser_t *abstract, double density)
{
	deepblu_cosmiq_parser_t *parser = (deepblu_cosmiq_parser_t *) abstract;

	parser->hydrostatic = density * GRAVITY;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (datetime) {
		datetime->year = array_uint16_le(data + 6);
		datetime->day = data[8];
		datetime->month = data[9];
		datetime->minute = data[10];
		datetime->hour = data[11];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	deepblu_cosmiq_parser_t *parser = (deepblu_cosmiq_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int mode = data[2];
	unsigned int atmospheric = array_uint16_le (data + 4) & 0x1FFF;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (mode == SCUBA || mode == GAUGE)
				*((unsigned int *) value) = array_uint16_le(data + 12) * 60;
			else
				*((unsigned int *) value) = array_uint16_le(data + 12);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (signed int) (array_uint16_le (data + 22) - atmospheric) * (BAR / 1000.0) / parser->hydrostatic;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = mode == SCUBA;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->oxygen = data[3] / 100.0;
			gasmix->helium = 0.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			switch (mode) {
			case SCUBA:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				ERROR (abstract->context, "Unknown activity type '%02x'", mode);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = atmospheric / 1000.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	deepblu_cosmiq_parser_t *parser = (deepblu_cosmiq_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int interval = data[26];
	unsigned int atmospheric = array_uint16_le (data + 4) & 0x1FFF;

	unsigned int time = 0;
	unsigned int offset = SZ_HEADER;
	while (offset + SZ_SAMPLE <= size) {
		dc_sample_value_t sample = {0};
		unsigned int temperature = array_uint16_le(data + offset + 0);
		unsigned int depth = array_uint16_le(data + offset + 2);
		offset += SZ_SAMPLE;

		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		sample.depth = (signed int) (depth - atmospheric) * (BAR / 1000.0) / parser->hydrostatic;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		sample.temperature = temperature / 10.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
	}

	return DC_STATUS_SUCCESS;
}
