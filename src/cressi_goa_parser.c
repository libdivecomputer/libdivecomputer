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
#include <stdbool.h>

#include "cressi_goa.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

/* One major difference between 'libdc' of Subsurface and upstream
 * 'libdivecomputer' is the open discussion on String-based interfaces.
 * Let's (ab)use this to determine whether we're building for Subsurface.
 */
#ifdef SAMPLE_EVENT_STRING
/* Subsurface currently only handles sample rates of equal seconds.
 * We need to make sure that we only report events on equal seconds.
 */
#define SUBSURFACE_ACCEPTABLE(interval, time) (((interval) != 1) || (((time) % 2) == 0))
#define SUBSURFACE_CORRECTION(interval, time) (((interval) == 1) && (((time) % 2) == 1))
#else
/* libdivecomputer can deal with sub-second sample rates. */
#define SUBSURFACE_ACCEPTABLE(interval, time) (true)
#define SUBSURFACE_CORRECTION(interval, time) (false)
#endif

#define ISINSTANCE(parser) dc_device_isinstance((parser), &cressi_goa_parser_vtable)

#define SZ_HEADER          23

#define DEPTH_SCUBA 0
#define DEPTH_FREE  1
#define SURFACE     2
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
	unsigned int version;
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

static const cressi_goa_layout_t layouts_original[] = {
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
		0, /* version */
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
		0, /* version */
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
		0, /* version */
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
		0, /* version */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t scuba_nitrox_layouts[] = {
	/* v0 */
	{
		90, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ 26, 28, UNDEFINED }, /* gasmix[0..2] */
		23, /* atmospheric */
		73, /* maxdepth */
		75, /* avgdepth */
		77, /* temperature */
		0, /* version */
		UNDEFINED, /* data_start */
	},
	/* v1-v2 */
	{
		92, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ 26, 28, UNDEFINED }, /* gasmix[0..2] */
		23, /* atmospheric */
		73, /* maxdepth */
		75, /* avgdepth */
		77, /* temperature */
		UNDEFINED, /* version */
		UNDEFINED, /* data_start */
	},
	/* v3 */
	{
		92, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ 26, 28, 87 }, /* gasmix[0..2] */
		23, /* atmospheric */
		73, /* maxdepth */
		75, /* avgdepth */
		77, /* temperature */
		3, /* version */
		UNDEFINED, /* data_start */
	},
	/* v4 */
	{
		82, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ 17, 19, 21 }, /* gasmix[0..2] */
		23, /* atmospheric */
		66, /* maxdepth */
		68, /* avgdepth */
		70, /* temperature */
		4, /* version */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t freedive_layouts[] = {
	/* v0 */
	{
		34, /* headersize */
		12, /* datetime */
		20, /* sessiontime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		23, /* maxdepth */
		UNDEFINED, /* avgdepth */
		25, /* temperature */
		0, /* version */
		UNDEFINED, /* data_start */
	},
	/* v1-v3 */
	{
		38, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		23, /* maxdepth */
		UNDEFINED, /* avgdepth */
		25, /* temperature */
		UNDEFINED, /* version */
		UNDEFINED, /* data_start */
	},
	/* v4 */
	{
		27, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		UNDEFINED, /* atmospheric */
		15, /* maxdepth */
		UNDEFINED, /* avgdepth */
		17, /* temperature */
		4, /* version */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t gauge_layouts[] = {
	/* v0 */
	{
		38, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		22, /* atmospheric */
		24, /* maxdepth */
		26, /* avgdepth */
		28, /* temperature */
		0, /* version */
		UNDEFINED, /* data_start */
	},
	/* v1-v3 */
	{
		38, /* headersize */
		12, /* datetime */
		20, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		22, /* atmospheric */
		24, /* maxdepth */
		26, /* avgdepth */
		28, /* temperature */
		UNDEFINED, /* version */
		UNDEFINED, /* data_start */
	},
	/* v4 */
	{
		28, /* headersize */
		4, /* datetime */
		11, /* divetime */
		{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
		13, /* atmospheric */
		15, /* maxdepth */
		17, /* avgdepth */
		19, /* temperature */
		4, /* version */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t advanced_freedive_layouts[] = {
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
		4, /* version */
		UNDEFINED, /* data_start */
	},
};

static const cressi_goa_layout_t *layouts_v0[] = {
	/* SCUBA */
	&scuba_nitrox_layouts[0],
	/* NITROX */
	&scuba_nitrox_layouts[0],
	/* FREEDIVE */
	&freedive_layouts[0],
	/* GAUGE */
	&gauge_layouts[0],
};

static const cressi_goa_layout_t *layouts_v1[] = {
	/* SCUBA */
	&scuba_nitrox_layouts[1],
	/* NITROX */
	&scuba_nitrox_layouts[1],
	/* FREEDIVE */
	&freedive_layouts[1],
	/* GAUGE */
	&gauge_layouts[1],
};

static const cressi_goa_layout_t *layouts_v2[] = {
	/* SCUBA */
	&scuba_nitrox_layouts[1],
	/* NITROX */
	&scuba_nitrox_layouts[1],
	/* FREEDIVE */
	&freedive_layouts[1],
	/* GAUGE */
	&gauge_layouts[1],
};

static const cressi_goa_layout_t *layouts_v3[] = {
	/* SCUBA */
	&scuba_nitrox_layouts[2],
	/* NITROX */
	&scuba_nitrox_layouts[2],
	/* FREEDIVE */
	&freedive_layouts[1],
	/* GAUGE */
	&gauge_layouts[1],
};

static const cressi_goa_layout_t *layouts_v4[] = {
	/* SCUBA */
	&scuba_nitrox_layouts[3],
	/* NITROX */
	&scuba_nitrox_layouts[3],
	/* FREEDIVE */
	&freedive_layouts[2],
	/* GAUGE */
	&gauge_layouts[2],
	/* Undefined */
	NULL,
	/* Advanced FREEDIVE */
	&advanced_freedive_layouts[0],
};

const struct {
	const cressi_goa_layout_t **layout;
	size_t size;
} layouts[] = {
	{ layouts_v0, C_ARRAY_SIZE(layouts_v0) },
	{ layouts_v1, C_ARRAY_SIZE(layouts_v1) },
	{ layouts_v2, C_ARRAY_SIZE(layouts_v2) },
	{ layouts_v3, C_ARRAY_SIZE(layouts_v3) },
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

static int
cressi_goa_irda_api_version(dc_parser_t *abstract, dc_event_devinfo_t *devinfo)
{
	if (devinfo->model > 11) {
		ERROR (abstract->context, "Unknown model %d.", devinfo->model);
		return -1;
	}
	int version;
	if (devinfo->firmware >= 161 && devinfo->firmware <= 165) {
		version = 0;
	} else if (devinfo->firmware >= 166 && devinfo->firmware <= 169) {
		version = 1;
	} else if (devinfo->firmware >= 170 && devinfo->firmware <= 179) {
		version = 2;
	} else if (devinfo->firmware >= 100 && devinfo->firmware <= 110) {
		version = 3;
	} else if ((devinfo->firmware >= 200 && devinfo->firmware <= 205)
			|| devinfo->firmware >= 900) {
		version = 4;
	} else {
		ERROR (abstract->context, "Unknown firmware version %d.", devinfo->firmware);
		return -1;
	}
	const bool version_support_on_model[5][11] = {
		/*  1      2      3      4      5      6      7      8      9      10     11  */
		{  true,  true, false, false, false, false, false, false, false, false, false }, /* API v0 */
		{  true,  true, false, false, false, false, false, false, false, false, false }, /* API v1 */
		{  true,  true, false, false, false, false, false, false,  true, false, false }, /* API v2 */
		{  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true }, /* API v3 */
		{  true,  true, false,  true,  true, false, false, false,  true,  true, false }, /* API v4 */
	};
	if (!version_support_on_model[version][devinfo->model - 1]) {
		ERROR (abstract->context, "Firmware version %d of Model %d not known to have support for API v%d.",
				devinfo->firmware, devinfo->model, version);
		return -1;
	}
	return version;
}

static dc_status_t cressi_goa_get_layout(dc_parser_t *abstract, unsigned int *divemode, cressi_goa_layout_t *layout)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	const unsigned int header_len = 4u;

	if (size < header_len)
		return DC_STATUS_DATAFORMAT;

	unsigned int divemode_;

	if (data[0] == 0xdc && data[1] == 0xdc) {
		const unsigned int id_len = data[3];
		/* data[2] contains the package format version and we currently only support version 0. */
		if (data[2] != 0x00 || size < header_len + id_len) {
			return DC_STATUS_DATAFORMAT;
		}
		const unsigned char *id_data = data + header_len;
		int version;
		if (id_len == 11) {
			version = array_uint16_le (id_data + 9);
		} else {
			dc_event_devinfo_t devinfo;
			devinfo.model = id_data[4];
			devinfo.firmware = array_uint16_le (id_data + 5);
			devinfo.serial = array_uint32_le (id_data + 0);

			version = cressi_goa_irda_api_version(abstract, &devinfo);
			if (version < 0) {
				return DC_STATUS_UNSUPPORTED;
			}
		}
		if ((size_t)version >= C_ARRAY_SIZE(layouts)) {
			return DC_STATUS_UNSUPPORTED;
		}

		const unsigned char *logbook = id_data + id_len;
		const unsigned int logbook_len = version < 4 ? 23 : 15;

		if (size < header_len + id_len + logbook_len) {
			return DC_STATUS_DATAFORMAT;
		}

		divemode_ = logbook[2];
		if (divemode_ >= layouts[version].size) {
			return DC_STATUS_UNSUPPORTED;
		}
		if (layouts[version].layout[divemode_] == NULL) {
			return DC_STATUS_UNSUPPORTED;
		}

		*layout = *(layouts[version].layout[divemode_]);
		layout->version = version;
		layout->data_start = header_len + id_len + logbook_len;
	} else {
		divemode_ = data[2];
		if (divemode_ >= C_ARRAY_SIZE(layouts_original)) {
			return DC_STATUS_DATAFORMAT;
		}
		*layout = layouts_original[divemode_];
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

static const unsigned int cressi_goa_time_multiplier = 500;

struct cressi_goa_parser_ctx {
	dc_parser_t *abstract;

	dc_sample_callback_t callback;
	void *userdata;

	bool last_depth_is_zero;
};

static void cressi_goa_parser_callback(dc_sample_type_t type, const dc_sample_value_t *value, struct cressi_goa_parser_ctx *ctx)
{
	if (!ctx->callback)
		return;
	if (type == DC_SAMPLE_DEPTH)
		ctx->last_depth_is_zero = value->depth == 0.0;
	ctx->callback(type, value, ctx->userdata);
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

	struct cressi_goa_parser_ctx ctx = {0};
	ctx.abstract = abstract;
	ctx.callback = callback;
	ctx.userdata = userdata;

	unsigned int interval = divemode == FREEDIVE ? 4 : 10;

	unsigned int offset = layout->data_start + layout->headersize;

	if (divemode == FREEDIVE_ADV) {
		const unsigned char *header = data + layout->data_start;
		unsigned int sample_rate = header[10];
		/* Valid sample rates are: 0.5s, 1s, 2s, 5s */
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
			case 4:
				interval = 10;
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
		size = offset + num_samples * 2;
	}

	unsigned int time = 0;
	unsigned int depth = 0;
	unsigned int gasmix = 0, gasmix_previous = 0xFFFFFFFF;
	unsigned int temperature = 0;
	bool have_temperature = false;
	bool complete = false;

	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;
		unsigned int t = 0;

		if (type == DEPTH_SCUBA) {
			depth = value & 0x7ffu;
			gasmix = (value >> 11) & ((layout->version < 3) ? 0x1u : 0x3u);
			/* speed_level = (layout->version < 3) ? (raw >> 14) & 0x3u : 0; */
			time += interval;
			complete = true;
		} else if (type == DEPTH_FREE) {
			depth = value & ((layout->version < 4) ? 0x7ffu : 0xfffu);
			time += interval;
			complete = true;
		} else if (type == TEMPERATURE) {
			temperature = value;
			have_temperature = true;
		} else if (type == SURFACE) {
			// SURFACE values are not given in the sample rate, but as seconds
			if (divemode == FREEDIVE_ADV && (offset + 2 < size)) {
				if (time)
					t = interval;
				if (SUBSURFACE_CORRECTION(interval, (time + t)))
					t = 2;
				time += t;
				depth = 0;

				// Time (seconds).
				sample.time = time * cressi_goa_time_multiplier;
				cressi_goa_parser_callback (DC_SAMPLE_TIME, &sample, &ctx);
				// Depth (1/10 m).
				sample.depth = 0.0;
				cressi_goa_parser_callback (DC_SAMPLE_DEPTH, &sample, &ctx);
			}
			// One `time` unit equals 500ms, so we have to multiply the `value` with 2 in order to count seconds
			time += ((value * 2) - t);
			if (divemode == FREEDIVE_ADV && t) {
				unsigned int correction = SUBSURFACE_CORRECTION(interval, time) ? 1 : 0;
				// Time (seconds).
				sample.time = (time - correction) * cressi_goa_time_multiplier;
				cressi_goa_parser_callback (DC_SAMPLE_TIME, &sample, &ctx);
				// Depth (1/10 m).
				sample.depth = 0.0;
				cressi_goa_parser_callback (DC_SAMPLE_DEPTH, &sample, &ctx);
			}
		}

		if (complete && SUBSURFACE_ACCEPTABLE(interval, time)) {
			// Time (seconds).
			sample.time = time * cressi_goa_time_multiplier;
			cressi_goa_parser_callback (DC_SAMPLE_TIME, &sample, &ctx);

			// Temperature (1/10 °C).
			if (have_temperature) {
				sample.temperature = temperature / 10.0;
				cressi_goa_parser_callback (DC_SAMPLE_TEMPERATURE, &sample, &ctx);
				have_temperature = false;
			}

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			cressi_goa_parser_callback (DC_SAMPLE_DEPTH, &sample, &ctx);

			// Gas change
			if (divemode == SCUBA || divemode == NITROX) {
				if (gasmix != gasmix_previous) {
					sample.gasmix = gasmix;
					cressi_goa_parser_callback (DC_SAMPLE_GASMIX, &sample, &ctx);
					gasmix_previous = gasmix;
				}
			}

			complete = false;
		}

		offset += 2;
	}

	if (!ctx.last_depth_is_zero) {
		dc_sample_value_t sample = {0};
		sample.time = time * cressi_goa_time_multiplier;
		cressi_goa_parser_callback (DC_SAMPLE_TIME, &sample, &ctx);
		sample.depth = 0.0;
		cressi_goa_parser_callback (DC_SAMPLE_DEPTH, &sample, &ctx);
	}

	return DC_STATUS_SUCCESS;
}
