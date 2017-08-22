/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Jef Driesen
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

#include "divesystem_idive.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &divesystem_idive_parser_vtable)

#define IX3M_EASY 0x22
#define IX3M_DEEP 0x23
#define IX3M_TEC  0x24
#define IX3M_REB  0x25

#define SZ_HEADER_IDIVE 0x32
#define SZ_SAMPLE_IDIVE 0x2A
#define SZ_HEADER_IX3M  0x36
#define SZ_SAMPLE_IX3M  0x36
#define SZ_SAMPLE_IX3M_APOS4 0x40

#define NGASMIXES 8

#define EPOCH 1199145600 /* 2008-01-01 00:00:00 */

#define OC       0
#define SCR      1
#define CCR      2
#define GAUGE    3
#define FREEDIVE 4
#define INVALID  0xFFFFFFFF

typedef struct divesystem_idive_parser_t divesystem_idive_parser_t;

struct divesystem_idive_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int headersize;
	// Cached fields.
	unsigned int cached;
	unsigned int divemode;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
};

static dc_status_t divesystem_idive_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t divesystem_idive_parser_vtable = {
	sizeof(divesystem_idive_parser_t),
	DC_FAMILY_DIVESYSTEM_IDIVE,
	divesystem_idive_parser_set_data, /* set_data */
	divesystem_idive_parser_get_datetime, /* datetime */
	divesystem_idive_parser_get_field, /* fields */
	divesystem_idive_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
divesystem_idive_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	divesystem_idive_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (divesystem_idive_parser_t *) dc_parser_allocate (context, &divesystem_idive_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	if (model >= IX3M_EASY) {
		parser->headersize = SZ_HEADER_IX3M;
	} else {
		parser->headersize = SZ_HEADER_IDIVE;
	}
	parser->cached = 0;
	parser->divemode = INVALID;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divemode = INVALID;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	dc_ticks_t ticks = array_uint32_le(abstract->data + 7) + EPOCH;

	if (!dc_datetime_localtime (datetime, ticks))
        return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		dc_status_t rc = divesystem_idive_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_ATMOSPHERIC:
			if (parser->model >= IX3M_EASY) {
				*((double *) value) = array_uint16_le (data + 11) / 10000.0;
			} else {
				*((double *) value) = array_uint16_le (data + 11) / 1000.0;
			}
			break;
		case DC_FIELD_SALINITY:
			water->type = data[34] == 0 ? DC_WATER_SALT : DC_WATER_FRESH;
			water->density = 0.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (parser->divemode == 0xFFFFFFFF)
				return DC_STATUS_UNSUPPORTED;
			switch (parser->divemode) {
			case OC:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case SCR:
			case CCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				ERROR (abstract->context, "Unknown dive mode %02x.", parser->divemode);
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
divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int maxdepth = 0;
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int o2_previous = 0xFFFFFFFF;
	unsigned int he_previous = 0xFFFFFFFF;
	unsigned int mode_previous = INVALID;
	unsigned int divemode = INVALID;

	unsigned int nsamples = array_uint16_le (data + 1);
	unsigned int samplesize = SZ_SAMPLE_IDIVE;
	if (parser->model >= IX3M_EASY) {
		// Detect the APOS4 firmware.
		unsigned int firmware = array_uint32_le(data + 0x2A);
		unsigned int apos4 = (firmware / 10000000) >= 4;
		if (apos4) {
			// Dive downloaded and recorded with the APOS4 firmware.
			samplesize = SZ_SAMPLE_IX3M_APOS4;
		} else if (size == parser->headersize + nsamples * SZ_SAMPLE_IX3M_APOS4) {
			// Dive downloaded with the APOS4 firmware, but recorded
			// with an older firmware.
			samplesize = SZ_SAMPLE_IX3M_APOS4;
		} else {
			// Dive downloaded and recorded with an older firmware.
			samplesize = SZ_SAMPLE_IX3M;
		}
	}

	unsigned int offset = parser->headersize;
	while (offset + samplesize <= size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		unsigned int timestamp = array_uint32_le (data + offset + 2);
		if (timestamp <= time) {
			ERROR (abstract->context, "Timestamp moved backwards.");
			return DC_STATUS_DATAFORMAT;
		}
		time = timestamp;
		sample.time = timestamp;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_le (data + offset + 6);
		if (maxdepth < depth)
			maxdepth = depth;
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (Celsius).
		signed int temperature = (signed short) array_uint16_le (data + offset + 8);
		sample.temperature = temperature / 10.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// Dive mode
		unsigned int mode = data[offset + 18];
		if (mode != mode_previous) {
			if (mode_previous != INVALID) {
				WARNING (abstract->context, "Dive mode changed from %02x to %02x.", mode_previous, mode);
			}
			mode_previous = mode;
		}
		if (divemode == INVALID) {
			divemode = mode;
		}

		// Setpoint
		if (mode == SCR || mode == CCR) {
			unsigned int setpoint = array_uint16_le (data + offset + 19);
			sample.setpoint = setpoint / 1000.0;
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
		}

		// Gaschange.
		unsigned int o2 = data[offset + 10];
		unsigned int he = data[offset + 11];
		if (o2 != o2_previous || he != he_previous) {
			// Find the gasmix in the list.
			unsigned int i = 0;
			while (i < ngasmixes) {
				if (o2 == oxygen[i] && he == helium[i])
					break;
				i++;
			}

			// Add it to list if not found.
			if (i >= ngasmixes) {
				if (i >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				oxygen[i] = o2;
				helium[i] = he;
				ngasmixes = i + 1;
			}

			sample.gasmix = i;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int deco = array_uint16_le (data + offset + 21);
		unsigned int tts  = array_uint16_le (data + offset + 23);
		if (tts != 0xFFFF) {
			if (deco) {
				sample.deco.type = DC_DECO_DECOSTOP;
				sample.deco.depth = deco / 10.0;
			} else {
				sample.deco.type = DC_DECO_NDL;
				sample.deco.depth = 0.0;
			}
			sample.deco.time = tts;
			if (callback) callback (DC_SAMPLE_DECO, sample, userdata);
		}

		// CNS
		unsigned int cns = array_uint16_le (data + offset + 29);
		sample.cns = cns / 100.0;
		if (callback) callback (DC_SAMPLE_CNS, sample, userdata);

		offset += samplesize;
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->helium[i] = helium[i];
		parser->oxygen[i] = oxygen[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->maxdepth = maxdepth;
	parser->divetime = time;
	parser->divemode = divemode;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
