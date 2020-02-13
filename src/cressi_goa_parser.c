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

#include "cressi_goa.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &cressi_goa_parser_vtable)

#define SZ_HEADER 0x5C

#define DEPTH       0
#define TEMPERATURE 3

typedef struct cressi_goa_parser_t cressi_goa_parser_t;

struct cressi_goa_parser_t {
	dc_parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	double maxdepth;
};

static dc_status_t cressi_goa_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t cressi_goa_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cressi_goa_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t cressi_goa_parser_vtable = {
	sizeof(cressi_goa_parser_t),
	DC_FAMILY_CRESSI_GOA,
	cressi_goa_parser_set_data, /* set_data */
	cressi_goa_parser_get_datetime, /* datetime */
	cressi_goa_parser_get_field, /* fields */
	cressi_goa_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
cressi_goa_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	cressi_goa_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cressi_goa_parser_t *) dc_parser_allocate (context, &cressi_goa_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->model = model;
	parser->cached = 0;
	parser->maxdepth = 0.0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->maxdepth = 0.0;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year = array_uint16_le(p + 0x0C);
		datetime->month = p[0x0E];
		datetime->day = p[0x0F];
		datetime->hour = p[0x10];
		datetime->minute = p[0x11];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;

	if (!parser->cached) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		dc_status_t rc = cressi_goa_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		parser->cached = 1;
		parser->maxdepth = statistics.maxdepth;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + 0x14);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 2;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = data[0x1B + 2 * flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = 5;
	unsigned int complete = 1;
	unsigned int gasmix_previous = 0xFFFFFFFF;

	unsigned int offset = SZ_HEADER;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;

		if (complete) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);
			complete = 0;
		}

		if (type == DEPTH) {
			// Depth (1/10 m).
			unsigned int depth = value & 0x07FF;
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Gas change.
			unsigned int gasmix = (value & 0x0800) >> 11;
			if (gasmix != gasmix_previous) {
				sample.gasmix = gasmix;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				gasmix_previous = gasmix;
			}

			complete = 1;
		} else if (type == TEMPERATURE) {
			// Temperature (1/10 °C).
			sample.temperature = value / 10.0;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
		} else {
			WARNING(abstract->context, "Unknown sample type %u.", type);
		}

		offset += 2;
	}

	return DC_STATUS_SUCCESS;
}
