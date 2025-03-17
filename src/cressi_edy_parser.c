/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#include "cressi_edy.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &cressi_edy_parser_vtable)

#define ARCHIMEDE 0x01
#define IQ700 0x05
#define EDY   0x08

#define SZ_HEADER 32

typedef struct cressi_edy_parser_t cressi_edy_parser_t;

typedef struct cressi_edy_layout_t {
	unsigned int datetime_y;
	unsigned int datetime_md;
	unsigned int datetime_hm;
	unsigned int avgdepth;
	unsigned int maxdepth;
	unsigned int temperature;
	unsigned int divetime;
	unsigned int gasmix;
	unsigned int gasmix_count;
} cressi_edy_layout_t;

struct cressi_edy_parser_t {
	dc_parser_t base;
	unsigned int model;
	const cressi_edy_layout_t *layout;
};

static dc_status_t cressi_edy_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t cressi_edy_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cressi_edy_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t cressi_edy_parser_vtable = {
	sizeof(cressi_edy_parser_t),
	DC_FAMILY_CRESSI_EDY,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	cressi_edy_parser_get_datetime, /* datetime */
	cressi_edy_parser_get_field, /* fields */
	cressi_edy_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const cressi_edy_layout_t edy = {
	 8, /* datetime_y */
	10, /* datetime_md */
	28, /* datetime_hm */
	 1, /* avgdepth */
	 5, /* maxdepth */
	22, /* temperature */
	25, /* divetime */
	46, 3, /* gasmix */
};

static const cressi_edy_layout_t archimede = {
	 2, /* datetime_y */
	 5, /* datetime_md */
	25, /* datetime_hm */
	22, /* avgdepth */
	 9, /* maxdepth */
	45, /* temperature */
	29, /* divetime */
	43, 1, /* gasmix */
};

static unsigned int
decode (const unsigned char data[], unsigned int offset, unsigned int n)
{
	unsigned int result = 0;

	for (unsigned int i = 0; i < n; ++i) {
		unsigned char byte = data[offset / 2];

		unsigned char nibble = 0;
		if ((offset & 1) == 0) {
			nibble = (byte >> 4) & 0x0F;
		} else {
			nibble = byte & 0x0F;
		}

		result *= 10;
		result += nibble;
		offset++;
	}

	return result;
}

static unsigned int
cressi_edy_parser_count_gasmixes (const unsigned char data[], const cressi_edy_layout_t *layout)
{
	// Count the number of active gas mixes. The active gas
	// mixes are always first, so we stop counting as soon
	// as the first gas marked as disabled is found.
	unsigned int i = 0;
	while (i < layout->gasmix_count) {
		unsigned int state = decode(data, layout->gasmix - i * 2, 1);
		if (state == 0x0F)
			break;
		i++;
	}
	return i;
}

dc_status_t
cressi_edy_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	cressi_edy_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cressi_edy_parser_t *) dc_parser_allocate (context, &cressi_edy_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;

	if (model == ARCHIMEDE) {
		parser->layout = &archimede;
	} else {
		parser->layout = &edy;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	cressi_edy_parser_t *parser = (cressi_edy_parser_t *) abstract;
	const cressi_edy_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (datetime) {
		datetime->year   = decode(data, layout->datetime_y, 2) + 2000;
		datetime->month  = decode(data, layout->datetime_md, 1);
		datetime->day    = decode(data, layout->datetime_md + 1, 2);
		datetime->hour   = decode(data, layout->datetime_hm, 2);
		datetime->minute = decode(data, layout->datetime_hm + 2, 2);
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	cressi_edy_parser_t *parser = (cressi_edy_parser_t *) abstract;
	const cressi_edy_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->model == EDY)
				*((unsigned int *) value) = decode(data, layout->divetime, 1) * 60 + decode(data, layout->divetime + 1, 2);
			else
				*((unsigned int *) value) = decode(data, layout->divetime, 3) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = decode(data, layout->maxdepth, 3) / 10.0;
			break;
		case DC_FIELD_AVGDEPTH:
			*((double *) value) = decode(data, layout->avgdepth, 3) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = cressi_edy_parser_count_gasmixes(data, layout);
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			gasmix->oxygen = decode(data, layout->gasmix - flags * 2, 2) / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = decode(data, layout->temperature, 3) / 10.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	cressi_edy_parser_t *parser = (cressi_edy_parser_t *) abstract;
	const cressi_edy_layout_t *layout = parser->layout;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = 30;
	if (parser->model == EDY) {
		interval = 1;
	} else if (parser->model == IQ700) {
		if (data[0x07] & 0x40)
			interval = 15;
	}

	unsigned int ngasmixes = cressi_edy_parser_count_gasmixes(data, layout);
	unsigned int gasmix = 0xFFFFFFFF;

	unsigned int offset = SZ_HEADER;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		if (data[offset] == 0xFF)
			break;

		unsigned int extra = 0;
		if (data[offset] & 0x80)
			extra = 4;

		// Time (seconds).
		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = decode(data + offset, 1, 3);
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Current gasmix
		if (ngasmixes) {
			unsigned int idx = (data[offset + 0] & 0x60) >> 5;
			if (parser->model == IQ700 || parser->model == ARCHIMEDE)
				idx = 0; /* FIXME */
			if (idx >= ngasmixes) {
				ERROR (abstract->context, "Invalid gas mix index.");
				return DC_STATUS_DATAFORMAT;
			}
			if (idx != gasmix) {
				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix = idx;
			}
		}

		offset += 2 + extra;
	}

	return DC_STATUS_SUCCESS;
}
