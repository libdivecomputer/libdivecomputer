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

#include <libdivecomputer/units.h>

#include "diverite_nitekq.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &diverite_nitekq_parser_vtable)

#define SZ_LOGBOOK 6

#define NGASMIXES 7

typedef struct diverite_nitekq_parser_t diverite_nitekq_parser_t;

struct diverite_nitekq_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	dc_divemode_t divemode;
	unsigned int metric;
	unsigned int ngasmixes;
	unsigned int o2[NGASMIXES];
	unsigned int he[NGASMIXES];
	unsigned int divetime;
	double maxdepth;
};

static dc_status_t diverite_nitekq_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t diverite_nitekq_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t diverite_nitekq_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t diverite_nitekq_parser_vtable = {
	sizeof(diverite_nitekq_parser_t),
	DC_FAMILY_DIVERITE_NITEKQ,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	diverite_nitekq_parser_get_datetime, /* datetime */
	diverite_nitekq_parser_get_field, /* fields */
	diverite_nitekq_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
diverite_nitekq_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	diverite_nitekq_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (diverite_nitekq_parser_t *) dc_parser_allocate (context, &diverite_nitekq_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->divemode = DC_DIVEMODE_OC;
	parser->metric = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->o2[i] = 0;
		parser->he[i] = 0;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_LOGBOOK)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year = p[0] + 2000;
		datetime->month = p[1];
		datetime->day = p[2];
		datetime->hour = p[3];
		datetime->minute = p[4];
		datetime->second = p[5];
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	diverite_nitekq_parser_t *parser = (diverite_nitekq_parser_t *) abstract;

	if (abstract->size < SZ_LOGBOOK)
		return DC_STATUS_DATAFORMAT;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (!parser->cached) {
		dc_status_t rc = diverite_nitekq_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			if (parser->metric)
				*((double *) value) = parser->maxdepth / 10.0;
			else
				*((double *) value) = parser->maxdepth * FEET / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = parser->he[flags] / 100.0;
			gasmix->oxygen = parser->o2[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = parser->divemode;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	diverite_nitekq_parser_t *parser = (diverite_nitekq_parser_t *) abstract;

	if (abstract->size < SZ_LOGBOOK)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data + SZ_LOGBOOK;
	unsigned int size = abstract->size - SZ_LOGBOOK;

	unsigned int type = 0;
	unsigned int metric = 0;
	unsigned int interval = 0;
	unsigned int maxdepth = 0;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int ngasmixes = 0;
	unsigned int gasmix = 0xFFFFFFFF; /* initialize with impossible value */
	unsigned int gasmix_previous = gasmix;
	dc_divemode_t divemode = DC_DIVEMODE_OC;

	unsigned int time = 0;
	unsigned int offset = 0;
	while (offset + 2 <= size) {
		if (data[offset] == 0xFF) {
			unsigned int o2 = 0, he = 0;
			unsigned int i = 0;

			type = data[offset + 1];
			switch (type) {
			case 0x01: // Settings
				if (offset + 27 > size)
					return DC_STATUS_DATAFORMAT;
				metric = (data[offset + 0x10] & 0x04) >> 2;
				interval = data[offset + 0x11];
				offset += 27;
				break;
			case 0x02: // OC Samples
			case 0x03: // CC Samples
				offset += 2;
				break;
			case 0x04: // Gas Change
				if (offset + 7 > size)
					return DC_STATUS_DATAFORMAT;

				// Get the new gas mix.
				o2 = data[offset + 5];
				he = data[offset + 6];

				// Find the gasmix in the list.
				i = 0;
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

				// Remember the index.
				gasmix = i;
				offset += 7;
				break;
			default:
				ERROR (abstract->context, "Unknown type %02x", type);
				return DC_STATUS_DATAFORMAT;
			}
		} else if (type == 2 || type == 3) {
			dc_sample_value_t sample = {0};

			if (interval == 0) {
				ERROR (abstract->context, "No sample interval present.");
				return DC_STATUS_DATAFORMAT;
			}

			// Time (seconds).
			time += interval;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Gas change
			if (gasmix != gasmix_previous) {
				sample.gasmix = gasmix;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix_previous = gasmix;
			}

			// Depth (1/10 m or ft).
			unsigned int depth = array_uint16_be (data + offset);
			if (maxdepth < depth)
				maxdepth = depth;
			if (metric)
				sample.depth = depth / 10.0;
			else
				sample.depth = depth * FEET / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			offset += 2;

			if (type == 3) {
				// Dive mode
				divemode = DC_DIVEMODE_CCR;

				// PPO2
				if (offset + 1 > size)
					return DC_STATUS_DATAFORMAT;
				unsigned int ppo2 = data[offset];
				sample.ppo2.sensor = DC_SENSOR_NONE;
				sample.ppo2.value = ppo2 / 100.0;
				if (callback) callback (DC_SAMPLE_PPO2, &sample, userdata);
				offset++;
			}
		} else {
			ERROR (abstract->context, "Invalid sample type %02x.", type);
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->he[i] = helium[i];
		parser->o2[i] = oxygen[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->maxdepth = maxdepth;
	parser->divetime = time;
	parser->metric = metric;
	parser->divemode = divemode;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
