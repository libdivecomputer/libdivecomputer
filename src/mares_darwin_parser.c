/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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
#include <string.h>

#include <libdivecomputer/units.h>

#include "mares_darwin.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &mares_darwin_parser_vtable)

#define DARWIN    0
#define DARWINAIR 1

#define AIR    0
#define GAUGE  1
#define NITROX 2

typedef struct mares_darwin_parser_t mares_darwin_parser_t;

struct mares_darwin_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int headersize;
	unsigned int samplesize;
};

static dc_status_t mares_darwin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_darwin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_darwin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t mares_darwin_parser_vtable = {
	sizeof(mares_darwin_parser_t),
	DC_FAMILY_MARES_DARWIN,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	mares_darwin_parser_get_datetime, /* datetime */
	mares_darwin_parser_get_field, /* fields */
	mares_darwin_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
mares_darwin_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	mares_darwin_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (mares_darwin_parser_t *) dc_parser_allocate (context, &mares_darwin_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->model = model;

	if (model == DARWINAIR) {
		parser->headersize = 60;
		parser->samplesize = 3;
	} else {
		parser->headersize = 52;
		parser->samplesize = 2;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_darwin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	mares_darwin_parser_t *parser = (mares_darwin_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = array_uint16_be (p);
		datetime->month  = p[2];
		datetime->day    = p[3];
		datetime->hour   = p[4];
		datetime->minute = p[5];
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_darwin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	mares_darwin_parser_t *parser = (mares_darwin_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;

	unsigned int mode = p[0x0C] & 0x03;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (p + 0x06) * 20;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_be (p + 0x08) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			if (mode == GAUGE) {
				*((unsigned int *) value) = 0;
			} else {
				*((unsigned int *) value) = 1;
			}
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			if (mode == NITROX) {
				gasmix->oxygen = p[0x0E] / 100.0;
			} else {
				gasmix->oxygen = 0.21;
			}
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed char) p[0x0A];
			break;
		case DC_FIELD_TANK_COUNT:
			if (parser->model == DARWINAIR) {
				*((unsigned int *) value) = 1;
			} else {
				*((unsigned int *) value) = 0;
			}
			break;
		case DC_FIELD_TANK:
			if (parser->model == DARWINAIR) {
				tank->type = DC_TANKVOLUME_METRIC;
				tank->volume = p[0x13] / 10.0;
				tank->workpressure = 0.0;
				tank->gasmix = 0;
				tank->beginpressure = array_uint16_be (p + 0x17);
				tank->endpressure = array_uint16_be (p + 0x19);
				tank->usage = DC_USAGE_NONE;
			} else {
				return DC_STATUS_UNSUPPORTED;
			}
			break;
		case DC_FIELD_DIVEMODE:
			switch (mode) {
			case AIR:
			case NITROX:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
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
mares_darwin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	mares_darwin_parser_t *parser = (mares_darwin_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int time = 0;

	unsigned int mode = abstract->data[0x0C] & 0x03;
	unsigned int pressure = array_uint16_be (abstract->data + 0x17);

	unsigned int gasmix_previous = 0xFFFFFFFF;
	unsigned int gasmix = gasmix_previous;
	if (mode != GAUGE) {
		gasmix = 0;
	}

	unsigned int offset = parser->headersize;
	while (offset + parser->samplesize <= abstract->size) {
			dc_sample_value_t sample = {0};

			unsigned int value = array_uint16_le (abstract->data + offset);
			unsigned int depth = value & 0x07FF;
			unsigned int ascent = (value & 0xE000) >> 13;
			unsigned int violation = (value & 0x1000) >> 12;
			unsigned int deco = (value & 0x0800) >> 11;

			// Surface Time (seconds).
			time += 20;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Gas change.
			if (gasmix != gasmix_previous) {
				sample.gasmix = gasmix;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix_previous = gasmix;
			}

			// Ascent rate
			if (ascent) {
				sample.event.type = SAMPLE_EVENT_ASCENT;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = ascent;
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			}

			// Deco violation
			if (violation) {
				sample.event.type = SAMPLE_EVENT_CEILING;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			}

			// Deco stop
			if (deco) {
				sample.deco.type = DC_DECO_DECOSTOP;
			} else {
				sample.deco.type = DC_DECO_NDL;
			}
			sample.deco.time = 0;
			sample.deco.depth = 0.0;
			sample.deco.tts = 0;
			if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);

			if (parser->samplesize == 3) {
				unsigned int type = (time / 20 + 2) % 3;
				if (type == 0) {
					// Tank Pressure (bar)
					pressure -= abstract->data[offset + 2];
					sample.pressure.tank = 0;
					sample.pressure.value = pressure;
					if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
				}
			}

			offset += parser->samplesize;
	}

	return DC_STATUS_SUCCESS;
}
