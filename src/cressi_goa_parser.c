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

#define SZ_HEADER          23
#define SZ_HEADER_SCUBA    0x61
#define SZ_HEADER_FREEDIVE 0x2B
#define SZ_HEADER_GAUGE    0x2D

#define DEPTH       0
#define DEPTH2      1
#define TIME        2
#define TEMPERATURE 3

#define SCUBA       0
#define NITROX      1
#define FREEDIVE    2
#define GAUGE       3

#define NGASMIXES 2

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

static const unsigned int headersizes[] = {
	SZ_HEADER_SCUBA,
	SZ_HEADER_SCUBA,
	SZ_HEADER_FREEDIVE,
	SZ_HEADER_GAUGE,
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
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int divemode = data[2];
	if (divemode >= C_ARRAY_SIZE(headersizes)) {
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int headersize = headersizes[divemode];
	if (size < headersize)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + 0x11;

	if (datetime) {
		datetime->year = array_uint16_le(p);
		datetime->month = p[2];
		datetime->day = p[3];
		datetime->hour = p[4];
		datetime->minute = p[5];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int divemode = data[2];
	if (divemode >= C_ARRAY_SIZE(headersizes)) {
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int headersize = headersizes[divemode];
	if (size < headersize)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		dc_status_t rc = cressi_goa_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		parser->cached = 1;
		parser->maxdepth = statistics.maxdepth;
	}

	unsigned int ngasmixes = 0;
	if (divemode == SCUBA || divemode == NITROX) {
		for (unsigned int i = 0; i < NGASMIXES; ++i) {
			if (data[0x20 + 2 * i] == 0)
				break;
			ngasmixes++;
		}
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + 0x19);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = data[0x20 + 2 * flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			switch (divemode) {
			case SCUBA:
			case NITROX:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
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
cressi_goa_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	unsigned int divemode = data[2];
	if (divemode >= C_ARRAY_SIZE(headersizes)) {
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int headersize = headersizes[divemode];
	if (size < headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int interval = divemode == FREEDIVE ? 2 : 5;

	unsigned int time = 0;
	unsigned int depth = 0;
	unsigned int gasmix = 0, gasmix_previous = 0xFFFFFFFF;
	unsigned int temperature = 0;
	unsigned int have_temperature = 0;
	unsigned int complete = 0;

	unsigned int offset = headersize;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;

		if (type == DEPTH || type == DEPTH2) {
			depth =  (value & 0x07FF);
			gasmix = (value & 0x0800) >> 11;
			time += interval;
			complete = 1;
		} else if (type == TEMPERATURE) {
			temperature = value;
			have_temperature = 1;
		} else if (type == TIME) {
			time += value;
		}

		if (complete) {
			// Time (seconds).
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Temperature (1/10 °C).
			if (have_temperature) {
				sample.temperature = temperature / 10.0;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
				have_temperature = 0;
			}

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Gas change
			if (divemode == SCUBA || divemode == NITROX) {
				if (gasmix != gasmix_previous) {
					sample.gasmix = gasmix;
					if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
					gasmix_previous = gasmix;
				}
			}

			complete = 0;
		}

		offset += 2;
	}

	return DC_STATUS_SUCCESS;
}
