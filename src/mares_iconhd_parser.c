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

#include <libdivecomputer/mares_iconhd.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &mares_iconhd_parser_vtable)

#define ICONHD    0x14
#define ICONHDNET 0x15

#define NGASMIXES 3

#define AIR    0
#define GAUGE  1
#define NITROX 2

typedef struct mares_iconhd_parser_t mares_iconhd_parser_t;

struct mares_iconhd_parser_t {
	dc_parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	unsigned int footer;
	unsigned int samplesize;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
};

static dc_status_t mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t mares_iconhd_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_iconhd_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_iconhd_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t mares_iconhd_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t mares_iconhd_parser_vtable = {
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_parser_set_data, /* set_data */
	mares_iconhd_parser_get_datetime, /* datetime */
	mares_iconhd_parser_get_field, /* fields */
	mares_iconhd_parser_samples_foreach, /* samples_foreach */
	mares_iconhd_parser_destroy /* destroy */
};

static dc_status_t
mares_iconhd_parser_cache (mares_iconhd_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	unsigned int footersize = 0x5C;
	unsigned int samplesize = 8;
	if (parser->model == ICONHDNET) {
		footersize = 0x80;
		samplesize = 12;
	}

	if (size < 4) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int length = array_uint32_le (data);

	if (size < length || length < footersize + 4) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned char *p = data + length - footersize;

	// Gas mixes
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	unsigned int mode = p[0] & 0x03;
	if (mode == GAUGE) {
		ngasmixes = 0;
	} else if (mode == AIR) {
		oxygen[0] = 21;
		ngasmixes = 1;
	} else {
		// Count the number of active gas mixes. The active gas
		// mixes are always first, so we stop counting as soon
		// as the first gas marked as disabled is found.
		ngasmixes = 0;
		while (ngasmixes < NGASMIXES) {
			if (p[0x14 + ngasmixes * 4 + 1] & 0x80)
				break;
			oxygen[ngasmixes] = p[0x14 + ngasmixes * 4];
			ngasmixes++;
		}
	}

	// Cache the data for later use.
	parser->footer = length - footersize;
	parser->samplesize = samplesize;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
	}
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}


dc_status_t
mares_iconhd_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) malloc (sizeof (mares_iconhd_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &mares_iconhd_parser_vtable);

	// Set the default values.
	parser->model = model;
	parser->cached = 0;
	parser->footer = 0;
	parser->samplesize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->footer = 0;
	parser->samplesize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	const unsigned char *p = abstract->data + parser->footer + 6;

	if (datetime) {
		datetime->hour   = array_uint16_le (p + 0);
		datetime->minute = array_uint16_le (p + 2);
		datetime->second = 0;
		datetime->day    = array_uint16_le (p + 4);
		datetime->month  = array_uint16_le (p + 6) + 1;
		datetime->year   = array_uint16_le (p + 8) + 1900;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	const unsigned char *p = abstract->data + parser->footer;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (p + 0x02) * 5;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (p + 0x04) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->helium = 0.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_ATMOSPHERIC:
			// Pressure (1/8 millibar)
			*((double *) value) = array_uint16_le (p + 0x26) / 8000.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed short) array_uint16_le (p + 0x46) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = (signed short) array_uint16_le (p + 0x48) / 10.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	const unsigned char *data = abstract->data;

	unsigned int time = 0;
	unsigned int interval = 5;

	unsigned int offset = 4;
	unsigned int nsamples = 0;

	// Previous gas mix - initialize with impossible value
	unsigned int gasmix_previous = 0xFFFFFFFF;

	while (offset + parser->samplesize <= parser->footer) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_le (data + offset + 0);
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (1/10 °C).
		unsigned int temperature = array_uint16_le (data + offset + 2) & 0x0FFF;
		sample.temperature = temperature / 10.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// Current gas mix
		if (parser->ngasmixes > 0) {
			unsigned int gasmix = (data[offset + 3] & 0xF0) >> 4;
			if (gasmix >= parser->ngasmixes) {
				ERROR (abstract->context, "Invalid gas mix index.");
				return DC_STATUS_DATAFORMAT;
			}
			if (gasmix != gasmix_previous) {
				sample.event.type = SAMPLE_EVENT_GASCHANGE;
				sample.event.time = 0;
				sample.event.value = parser->oxygen[gasmix];
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
				gasmix_previous = gasmix;
			}
		}

		offset += parser->samplesize;
		nsamples++;

		// Some extra data.
		if (parser->model == ICONHDNET && (nsamples % 4) == 0) {
			if (offset + 8 > parser->footer) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}

			// Pressure (1/100 bar).
			unsigned int pressure = array_uint16_le(data + offset);
			sample.pressure.tank = 0;
			sample.pressure.value = pressure / 100.0;
			if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);

			offset += 8;
		}
	}

	return DC_STATUS_SUCCESS;
}
