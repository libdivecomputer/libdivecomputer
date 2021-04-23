/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Jef Driesen, David Carron
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

#include <string.h>
#include <stdlib.h>

#include <libdivecomputer/units.h>

#include "mclean_extreme.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &mclean_extreme_parser_vtable)

#define SZ_CFG              0x002D
#define SZ_COMPUTER         (SZ_CFG + 0x6A)
#define SZ_HEADER           (SZ_CFG + 0x31)
#define SZ_SAMPLE           0x0004

#define EPOCH 946684800 // 2000-01-01 00:00:00 UTC

#define REC   0
#define TEC   1
#define CCR   2
#define GAUGE 3

#define INVALID 0xFFFFFFFF

#define NGASMIXES 8

typedef struct mclean_extreme_parser_t mclean_extreme_parser_t;

struct mclean_extreme_parser_t {
	dc_parser_t base;

	// Cached fields.
	unsigned int cached;
	unsigned int ngasmixes;
	unsigned int gasmix[NGASMIXES];
};

static dc_status_t mclean_extreme_parser_get_datetime(dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mclean_extreme_parser_get_field(dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mclean_extreme_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t mclean_extreme_parser_vtable = {
	sizeof(mclean_extreme_parser_t),
	DC_FAMILY_MCLEAN_EXTREME,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	mclean_extreme_parser_get_datetime, /* datetime */
	mclean_extreme_parser_get_field, /* fields */
	mclean_extreme_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
mclean_extreme_parser_create(dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	mclean_extreme_parser_t *parser = NULL;

	if (out == NULL) {
		return DC_STATUS_INVALIDARGS;
	}

	// Allocate memory.
	parser = (mclean_extreme_parser_t *)dc_parser_allocate(context, &mclean_extreme_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i] = INVALID;
	}

	*out = (dc_parser_t *)parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_get_datetime(dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER) {
		ERROR(abstract->context, "Corrupt dive data");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int timestamp = array_uint32_le(abstract->data + SZ_CFG + 0x0000);
	dc_ticks_t ticks = (dc_ticks_t) timestamp + EPOCH;

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_get_field(dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	mclean_extreme_parser_t *parser = (mclean_extreme_parser_t *)abstract;

	if (abstract->size < SZ_HEADER) {
		ERROR(abstract->context, "Corrupt dive data");
		return DC_STATUS_DATAFORMAT;
	}

	if (!parser->cached) {
		dc_status_t rc = mclean_extreme_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *)value;
	dc_salinity_t *salinity = (dc_salinity_t *)value;

	const unsigned int atmospheric = array_uint16_le(abstract->data + 0x001E);
	const unsigned int density_index = abstract->data[0x0023];
	double density = 0;

	switch (density_index) {
	case 0:
		density = 1000.0;
		break;
	case 1:
		density = 1020.0;
		break;
	case 2:
		density = 1030.0;
		break;
	default:
		ERROR(abstract->context, "Corrupt density index in dive data");
		return DC_STATUS_DATAFORMAT;
	}

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *)value) = array_uint32_le(abstract->data + SZ_CFG + 0x000C) - array_uint32_le(abstract->data + SZ_CFG + 0x0000);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *)value) = (signed int)(array_uint16_le(abstract->data + SZ_CFG + 0x0016) - atmospheric) * (BAR / 1000.0) / (density * GRAVITY);
			break;
		case DC_FIELD_AVGDEPTH:
			*((double *)value) = (signed int)(array_uint16_le(abstract->data + SZ_CFG + 0x0018) - atmospheric) * (BAR / 1000.0) / (density * GRAVITY);
			break;
		case DC_FIELD_SALINITY:
			salinity->density = density;
			salinity->type = density_index == 0 ? DC_WATER_FRESH : DC_WATER_SALT;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *)value) = atmospheric / 1000.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *)value) = (double)abstract->data[SZ_CFG + 0x0010];
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *)value) = (double)abstract->data[SZ_CFG + 0x0011];
			break;
		case DC_FIELD_DIVEMODE:
			switch (abstract->data[0x002C]) {
			case REC:
			case TEC:
				*((dc_divemode_t *)value) = DC_DIVEMODE_OC;
				break;
			case CCR:
				*((dc_divemode_t *)value) = DC_DIVEMODE_CCR;
				break;
			case GAUGE:
				*((dc_divemode_t *)value) = DC_DIVEMODE_GAUGE;
				break;
			default:
				ERROR(abstract->context, "Corrupt dive mode in dive data");
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *)value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.01 * abstract->data[0x0001 + 1 + 2 * parser->gasmix[flags]];
			gasmix->oxygen = 0.01 * abstract->data[0x0001 + 0 + 2 * parser->gasmix[flags]];
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	mclean_extreme_parser_t *parser = (mclean_extreme_parser_t *)abstract;

	if (abstract->size < SZ_HEADER) {
		ERROR(abstract->context, "Corrupt dive data");
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned int nsamples = array_uint16_le(abstract->data + 0x005C);

	if (abstract->size != SZ_HEADER + nsamples * SZ_SAMPLE) {
		ERROR(abstract->context, "Corrupt dive data");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int ngasmixes = 0;
	unsigned int gasmix[NGASMIXES] = {0};
	unsigned int gasmix_previous = INVALID;

	const unsigned int interval = 10;
	unsigned int time = 0;
	size_t offset = SZ_HEADER;
	for (unsigned int i = 0; i < nsamples; ++i) {
		dc_sample_value_t sample = { 0 };

		const unsigned int depth = array_uint16_le(abstract->data + offset + 0);
		const unsigned int temperature = abstract->data[offset + 2];
		const unsigned int flags = abstract->data[offset + 3];
		const unsigned int ccr = flags & 0x80;
		const unsigned int gasmix_id = (flags & 0x1C) >> 2;
		const unsigned int sp_index = (flags & 0x60) >> 5;
		const unsigned int setpoint = abstract->data[0x0013 + sp_index];

		time += interval;
		sample.time = time * 1000;
		if (callback) callback(DC_SAMPLE_TIME, &sample, userdata);

		sample.depth = 0.1 * depth;
		if (callback) callback(DC_SAMPLE_DEPTH, &sample, userdata);

		sample.temperature = temperature;
		if (callback) callback(DC_SAMPLE_TEMPERATURE, &sample, userdata);

		if (gasmix_id != gasmix_previous) {
			// Find the gasmix in the list.
			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (gasmix_id == gasmix[idx])
					break;
				idx++;
			}

			// Add it to list if not found.
			if (idx >= ngasmixes) {
				if (idx >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				gasmix[idx] = gasmix_id;
				ngasmixes = idx + 1;
			}

			sample.gasmix = idx;
			if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
			gasmix_previous = gasmix_id;
		}

		if (ccr) {
			sample.setpoint = 0.01 * setpoint;
			if (callback) callback(DC_SAMPLE_SETPOINT, &sample, userdata);
		}

		offset += SZ_SAMPLE;
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
