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

#include <stdlib.h>

#include <libdivecomputer/oceanic_veo250.h>
#include <libdivecomputer/units.h>

#include "oceanic_common.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &oceanic_veo250_parser_vtable)

#define REACTPRO 0x4247
#define VEO200   0x424B
#define VEO250   0x424C
#define REACTPROWHITE 0x4354

typedef struct oceanic_veo250_parser_t oceanic_veo250_parser_t;

struct oceanic_veo250_parser_t {
	dc_parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	double maxdepth;
};

static dc_status_t oceanic_veo250_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t oceanic_veo250_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t oceanic_veo250_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t oceanic_veo250_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t oceanic_veo250_parser_vtable = {
	sizeof(oceanic_veo250_parser_t),
	DC_FAMILY_OCEANIC_VEO250,
	oceanic_veo250_parser_set_data, /* set_data */
	oceanic_veo250_parser_get_datetime, /* datetime */
	oceanic_veo250_parser_get_field, /* fields */
	oceanic_veo250_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
oceanic_veo250_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	oceanic_veo250_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (oceanic_veo250_parser_t *) dc_parser_allocate (context, &oceanic_veo250_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) abstract;

	if (abstract->size < 8)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = ((p[5] & 0xF0) >> 4) + ((p[1] & 0xE0) >> 1) + 2000;
		datetime->month  = ((p[7] & 0xF0) >> 4);
		datetime->day    = p[1] & 0x1F;
		datetime->hour   = p[3];
		datetime->minute = p[2];
		datetime->second = 0;

		if (parser->model == VEO200 || parser->model == VEO250)
			datetime->year += 3;
		else if (parser->model == REACTPRO)
			datetime->year += 2;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		dc_status_t rc = oceanic_veo250_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		parser->cached = 1;
		parser->divetime = statistics.divetime;
		parser->maxdepth = statistics.maxdepth;
	}

	unsigned int footer = size - PAGESIZE;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = data[footer + 3] * 60 + data[footer + 4] * 3600;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth;
			break;
		case DC_FIELD_GASMIX_COUNT:
				*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			if (data[footer + 6])
				gasmix->oxygen = data[footer + 6] / 100.0;
			else
				gasmix->oxygen = 0.21;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	oceanic_veo250_parser_t *parser = (oceanic_veo250_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return DC_STATUS_DATAFORMAT;

	unsigned int time = 0;
	unsigned int interval = 0;
	unsigned int interval_idx = data[0x27] & 0x03;
	if (parser->model == REACTPRO || parser->model == REACTPROWHITE) {
		interval_idx += 1;
		interval_idx %= 4;
	}
	switch (interval_idx) {
	case 0:
		interval = 2;
		break;
	case 1:
		interval = 15;
		break;
	case 2:
		interval = 30;
		break;
	case 3:
		interval = 60;
		break;
	}

	unsigned int offset = 5 * PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Time.
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_VEO250;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (DC_SAMPLE_VENDOR, sample, userdata);

		// Depth (ft)
		unsigned int depth = data[offset + 2];
		sample.depth = depth * FEET;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (°F)
		unsigned int temperature;
		if (parser->model == REACTPRO || parser->model == REACTPROWHITE) {
			temperature = data[offset + 6];
		} else {
			temperature = data[offset + 7];
		}
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		offset += PAGESIZE / 2;
	}

	return DC_STATUS_SUCCESS;
}
