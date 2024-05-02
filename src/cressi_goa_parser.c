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

#define DEPTH       0
#define DEPTH2      1
#define TIME        2
#define TEMPERATURE 3

#define SCUBA        0
#define NITROX       1
#define FREEDIVE     2
#define GAUGE        3
#define FREEDIVE_ADV 5

#define NGASMIXES 3

#define UNDEFINED 0xFFFFFFFF

typedef struct cressi_goa_parser_t cressi_goa_parser_t;

struct cressi_goa_parser_t {
	dc_parser_t base;
	unsigned int model;
};

typedef struct cressi_goa_layout_t {
	unsigned int headersize;
	unsigned int datetime;
	unsigned int divetime;
	unsigned int gasmix[NGASMIXES];
	unsigned int atmospheric;
	unsigned int maxdepth;
	unsigned int avgdepth;
	unsigned int temperature;
	unsigned int data_start;
} cressi_goa_layout_t;

static dc_status_t cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t cressi_goa_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cressi_goa_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t cressi_goa_parser_vtable = {
	sizeof(cressi_goa_parser_t),
	DC_FAMILY_CRESSI_GOA,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	cressi_goa_parser_get_datetime, /* datetime */
	cressi_goa_parser_get_field, /* fields */
	cressi_goa_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const cressi_goa_layout_t layouts_v0[] = {
	/* SCUBA */
	{
		0x61, /* headersize */
		0x11, /* datetime */
		0x19, /* divetime */
		{ 0x1F, 0x21, UNDEFINED }, /* gasmix */
		0x23, /* atmospheric */
		0x4E, /* maxdepth */
		0x50, /* avgdepth */
		0x52, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* NITROX */
	{
		0x61, /* headersize */
		0x11, /* datetime */
		0x19, /* divetime */
		{ 0x1F, 0x21, UNDEFINED }, /* gasmix */
		0x23, /* atmospheric */
		0x4E, /* maxdepth */
		0x50, /* avgdepth */
		0x52, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* FREEDIVE */
	{
		0x2B, /* headersize */
		0x11, /* datetime */
		0x19, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		0x1C, /* maxdepth */
		UNDEFINED, /* avgdepth */
		0x1E, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* GAUGE */
	{
		0x2D, /* headersize */
		0x11, /* datetime */
		0x19, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		0x1B, /* atmospheric */
		0x1D, /* maxdepth */
		0x1F, /* avgdepth */
		0x21, /* temperature */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t layouts_v4[] = {
	/* SCUBA */
	{
		82, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ 17, 19, 21 }, /* gasmix[0..2] */
		23, /* atmospheric */
		66, /* maxdepth */
		68, /* avgdepth */
		70, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* NITROX */
	{
		82, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ 17, 19, 21 }, /* gasmix[0..2] */
		23, /* atmospheric */
		66, /* maxdepth */
		68, /* avgdepth */
		70, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* FREEDIVE */
	{
		27, /* headersize */
		4, /* datetime */
		21, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		15, /* maxdepth */
		UNDEFINED, /* avgdepth */
		17, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* GAUGE */
	{
		28, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		13, /* atmospheric */
		15, /* maxdepth */
		17, /* avgdepth */
		19, /* temperature */
		UNDEFINED, /* data_start */
	},
	/* Undefined */
	{ },
	/* Advanced FREEDIVE */
	{
		28, /* headersize */
		4, /* datetime */
		22, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		16, /* maxdepth */
		UNDEFINED, /* avgdepth */
		18, /* temperature */
		UNDEFINED, /* data_start */
	},
};

const struct {
	const cressi_goa_layout_t *layout;
	size_t size;
} layouts[] = {
	{ layouts_v0, C_ARRAY_SIZE(layouts_v0) },
	{ NULL, 0 },
	{ NULL, 0 },
	{ NULL, 0 },
	{ layouts_v4, C_ARRAY_SIZE(layouts_v4) },
};

dc_status_t
cressi_goa_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	cressi_goa_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cressi_goa_parser_t *) dc_parser_allocate (context, &cressi_goa_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->model = model;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t cressi_goa_get_layout(dc_parser_t *abstract, unsigned int *divemode, cressi_goa_layout_t *layout)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 4)
		return DC_STATUS_DATAFORMAT;

	cressi_goa_layout_t dynamic_layout;
	unsigned int divemode_;

	unsigned char version = data[2];
	if (data[0] == 0xdc && data[1] == 0xdc && (version == 0x00 || version == 0x04)) {
		if (version >= C_ARRAY_SIZE(layouts)) {
			return DC_STATUS_DATAFORMAT;
		}

		divemode_ = data[6];
		if (divemode_ >= layouts[version].size) {
			return DC_STATUS_DATAFORMAT;
		}

		*layout = layouts[version].layout[divemode_];
		layout->data_start = 4 + data[3];
	} else {
		divemode_ = data[2];
		if (divemode_ >= C_ARRAY_SIZE(layouts_v0)) {
			return DC_STATUS_DATAFORMAT;
		}
		*layout = layouts_v0[divemode_];
		layout->data_start = 0;
	}

	if (size < layout->headersize + layout->data_start)
		return DC_STATUS_DATAFORMAT;

	if (divemode)
		*divemode = divemode_;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	cressi_goa_layout_t dynamic_layout;
	dc_status_t status = cressi_goa_get_layout(abstract, NULL, &dynamic_layout);
	if (status != DC_STATUS_SUCCESS)
		return status;
	const cressi_goa_layout_t *layout = &dynamic_layout;

	const unsigned char *p = abstract->data + layout->data_start + layout->datetime;

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
	unsigned int divemode;
	cressi_goa_layout_t dynamic_layout;
	dc_status_t status = cressi_goa_get_layout(abstract, &divemode, &dynamic_layout);
	if (status != DC_STATUS_SUCCESS)
		return status;
	const cressi_goa_layout_t *layout = &dynamic_layout;
	const unsigned char *data = abstract->data + layout->data_start;

	unsigned int ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		if (layout->gasmix[i] == UNDEFINED)
			break;
		if (data[layout->gasmix[i] + 1] == 0)
			break;
		ngasmixes++;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (layout->divetime == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((unsigned int *) value) = array_uint16_le (data + layout->divetime);
			break;
		case DC_FIELD_MAXDEPTH:
			if (layout->maxdepth == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = array_uint16_le (data + layout->maxdepth) / 10.0;
			break;
		case DC_FIELD_AVGDEPTH:
			if (layout->avgdepth == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = array_uint16_le (data + layout->avgdepth) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			if (layout->temperature == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = array_uint16_le (data + layout->temperature) / 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			if (layout->atmospheric == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = array_uint16_le (data + layout->atmospheric) / 1000.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			if (ngasmixes <= flags)
				return DC_STATUS_INVALIDARGS;
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			gasmix->oxygen = data[layout->gasmix[flags] + 1] / 100.0;
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
			case FREEDIVE_ADV:
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

	unsigned int divemode;
	cressi_goa_layout_t dynamic_layout;
	dc_status_t status = cressi_goa_get_layout(abstract, &divemode, &dynamic_layout);
	if (status != DC_STATUS_SUCCESS)
		return status;
	const cressi_goa_layout_t *layout = &dynamic_layout;

	const unsigned int time_multiplier = 500;
	unsigned int interval = divemode == FREEDIVE ? 4 : 10;

	unsigned int time = 0;
	unsigned int depth = 0;
	unsigned int gasmix = 0, gasmix_previous = 0xFFFFFFFF;
	unsigned int temperature = 0;
	unsigned int have_temperature = 0;
	unsigned int complete = 0;

	unsigned int offset = layout->data_start + layout->headersize;

	if (divemode == FREEDIVE_ADV) {
		const unsigned char *header = data + layout->data_start;
		unsigned int sample_rate = header[10];
		switch (sample_rate) {
			case 1:
				interval = 1;
				break;
			case 2:
				interval = 2;
				break;
			case 3:
				interval = 4;
				break;
			default:
				ERROR (abstract->context, "Unknown sample rate: 0x%02x.", sample_rate);
				return DC_STATUS_DATAFORMAT;
		}

		/* Adjust the size in order to skip the Advanced Freedive Dip Stats that come at the end.
		 * If we'd ever want to use those, the structure is:
		 *
		 * SURFTIME | MAXDEPTH | DIPTIME | MINTEMP | SPEED_DESC | SPEED_ASC | TARAVANA_VIOLATION
		 *   u16    |   u16    |   u16   |   u16   |    u16     |    u16    |       bool
		 */
		unsigned int num_samples = array_uint16_le (header + 2);
		size = layout->data_start + layout->headersize + num_samples * 2;
	}

	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;
		unsigned int t = 0;

		if (type == DEPTH) {
			depth =  (value & 0x07FF);
			gasmix = (value & 0x0800) >> 11;
			time += interval;
			complete = 1;
		} else if (type == DEPTH2) {
			depth =  (value & 0x0FFF);
			gasmix = 0;
			time += interval;
			complete = 1;
		} else if (type == TEMPERATURE) {
			temperature = value;
			have_temperature = 1;
		} else if (type == TIME) {
			if (divemode == FREEDIVE_ADV) {
				if (time)
					t = 1;
				time += (t * interval);
				depth = 0;

				// Time (seconds).
				sample.time = time * time_multiplier;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
				// Depth (1/10 m).
				sample.depth = depth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			}
			time += (value - t) * interval;
			if (divemode == FREEDIVE_ADV && t) {
				// Time (seconds).
				sample.time = time * time_multiplier;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
				// Depth (1/10 m).
				sample.depth = depth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			}
		}

		if (complete) {
			// Time (seconds).
			sample.time = time * time_multiplier;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Temperature (1/10 °C).
			if (have_temperature) {
				sample.temperature = temperature / 10.0;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
				have_temperature = 0;
			}

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Gas change
			if (divemode == SCUBA || divemode == NITROX) {
				if (gasmix != gasmix_previous) {
					sample.gasmix = gasmix;
					if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
					gasmix_previous = gasmix;
				}
			}

			complete = 0;
		}

		offset += 2;
	}

	return DC_STATUS_SUCCESS;
}
