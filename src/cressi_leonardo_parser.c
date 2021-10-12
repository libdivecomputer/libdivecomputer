/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include "cressi_leonardo.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &cressi_leonardo_parser_vtable)

#define SZ_HEADER 82

#define DRAKE 6

typedef struct cressi_leonardo_parser_t cressi_leonardo_parser_t;

struct cressi_leonardo_parser_t {
	dc_parser_t base;
	unsigned int model;
};

static dc_status_t cressi_leonardo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t cressi_leonardo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t cressi_leonardo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cressi_leonardo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t cressi_leonardo_parser_vtable = {
	sizeof(cressi_leonardo_parser_t),
	DC_FAMILY_CRESSI_LEONARDO,
	cressi_leonardo_parser_set_data, /* set_data */
	cressi_leonardo_parser_get_datetime, /* datetime */
	cressi_leonardo_parser_get_field, /* fields */
	cressi_leonardo_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
cressi_leonardo_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	cressi_leonardo_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cressi_leonardo_parser_t *) dc_parser_allocate (context, &cressi_leonardo_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->model = model;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year = p[8] + 2000;
		datetime->month = p[9];
		datetime->day = p[10];
		datetime->hour = p[11];
		datetime->minute = p[12];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	cressi_leonardo_parser_t *parser = (cressi_leonardo_parser_t *) abstract;
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;

	unsigned int interval = 20;
	if (parser->model == DRAKE) {
		interval = data[0x17];
	}
	if (interval == 0) {
		ERROR(abstract->context, "Invalid sample interval");
		return DC_STATUS_DATAFORMAT;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + 0x06) * interval;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 0x20) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			if (parser->model == DRAKE) {
				*((unsigned int *) value) = 0;
			} else {
				*((unsigned int *) value) = 1;
			}
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = data[0x19] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = data[0x22];
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	cressi_leonardo_parser_t *parser = (cressi_leonardo_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = 20;
	if (parser->model == DRAKE) {
		interval = data[0x17];
	}
	if (interval == 0) {
		ERROR(abstract->context, "Invalid sample interval");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int gasmix_previous = 0xFFFFFFFF;
	unsigned int gasmix = 0;
	if (parser->model == DRAKE) {
		gasmix = gasmix_previous;
	}

	unsigned int offset = SZ_HEADER;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		if (offset + 4 <= size &&
			array_uint16_le (data + offset + 2) == 0xFF00)
		{
			unsigned int surftime = data[offset] + (data[offset + 1] & 0x07) * 60;

			// Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Depth (1/10 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			offset += 4;
		} else {
			unsigned int value = array_uint16_le (data + offset);
			unsigned int depth = value & 0x07FF;
			unsigned int ascent = (value & 0xC000) >> 14;

			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Gas change.
			if (gasmix != gasmix_previous) {
				sample.gasmix = gasmix;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				gasmix_previous = gasmix;
			}

			// Ascent rate
			if (ascent) {
				sample.event.type = SAMPLE_EVENT_ASCENT;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = ascent;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}

			offset += 2;
		}
	}

	return DC_STATUS_SUCCESS;
}
