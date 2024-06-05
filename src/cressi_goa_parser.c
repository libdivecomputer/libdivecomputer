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

#define DEPTH_SCUBA 0
#define DEPTH_FREE  1
#define SURFACE     2
#define TEMPERATURE 3

#define SCUBA        0
#define NITROX       1
#define FREEDIVE     2
#define GAUGE        3
#define FREEDIVE_ADV 5

#define NGASMIXES  3
#define NVERSIONS  5
#define NDIVEMODES 6

#define UNDEFINED 0xFFFFFFFF

typedef struct cressi_goa_parser_t cressi_goa_parser_t;

typedef struct cressi_goa_layout_t {
	unsigned int headersize;
	unsigned int datetime;
	unsigned int divetime;
	unsigned int gasmix[NGASMIXES];
	unsigned int atmospheric;
	unsigned int maxdepth;
	unsigned int avgdepth;
	unsigned int temperature;
	unsigned int samplerate;
	unsigned int samplenumber;
} cressi_goa_layout_t;

struct cressi_goa_parser_t {
	dc_parser_t base;
	const cressi_goa_layout_t *layout;
	unsigned int model;
	unsigned int version;
	unsigned int data_start;
	unsigned int divemode;
};

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
		UNDEFINED, /* samplerate */
		UNDEFINED, /* samplenumber */
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
		UNDEFINED, /* samplerate */
		UNDEFINED, /* samplenumber */
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
		UNDEFINED, /* samplerate */
		UNDEFINED, /* samplenumber */
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
		UNDEFINED, /* samplerate */
		UNDEFINED, /* samplenumber */
	},
};

static const cressi_goa_layout_t scuba_nitrox_layout_v0 = {
	90, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, UNDEFINED }, /* gasmix[0..2] */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t scuba_nitrox_layout_v1v2 = {
	92, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, UNDEFINED }, /* gasmix[0..2] */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t scuba_nitrox_layout_v3 = {
	92, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, 87 }, /* gasmix[0..2] */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t scuba_nitrox_layout_v4 = {
	82, /* headersize */
	4, /* datetime */
	11, /* divetime */
	{ 17, 19, 21 }, /* gasmix[0..2] */
	23, /* atmospheric */
	66, /* maxdepth */
	68, /* avgdepth */
	70, /* temperature */
	10, /* samplerate */
	2, /* samplenumber */
};

static const cressi_goa_layout_t freedive_layout_v0 = {
	34, /* headersize */
	12, /* datetime */
	20, /* sessiontime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	23, /* maxdepth */
	UNDEFINED, /* avgdepth */
	25, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t freedive_layout_v1v2v3 = {
	38, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	23, /* maxdepth */
	UNDEFINED, /* avgdepth */
	25, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t freedive_layout_v4 = {
	27, /* headersize */
	4, /* datetime */
	11, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	15, /* maxdepth */
	UNDEFINED, /* avgdepth */
	17, /* temperature */
	10, /* samplerate */
	2, /* samplenumber */
};

static const cressi_goa_layout_t gauge_layout_v0 = {
	38, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	22, /* atmospheric */
	24, /* maxdepth */
	26, /* avgdepth */
	28, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t gauge_layout_v1v2v3 = {
	38, /* headersize */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	22, /* atmospheric */
	24, /* maxdepth */
	26, /* avgdepth */
	28, /* temperature */
	UNDEFINED, /* samplerate */
	10, /* samplenumber */
};
static const cressi_goa_layout_t gauge_layout_v4 = {
	28, /* headersize */
	4, /* datetime */
	11, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	13, /* atmospheric */
	15, /* maxdepth */
	17, /* avgdepth */
	19, /* temperature */
	10, /* samplerate */
	2, /* samplenumber */
};

static const cressi_goa_layout_t advanced_freedive_layout_v4 = {
	28, /* headersize */
	4, /* datetime */
	22, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	16, /* maxdepth */
	UNDEFINED, /* avgdepth */
	18, /* temperature */
	10, /* samplerate */
	2, /* samplenumber */
};

static const cressi_goa_layout_t * const layouts[NVERSIONS][NDIVEMODES] = {
	{
		/* SCUBA */
		&scuba_nitrox_layout_v0,
		/* NITROX */
		&scuba_nitrox_layout_v0,
		/* FREEDIVE */
		&freedive_layout_v0,
		/* GAUGE */
		&gauge_layout_v0,
		/* Undefined */
		NULL,
		/* Undefined */
		NULL,
	},
	{
		/* SCUBA */
		&scuba_nitrox_layout_v1v2,
		/* NITROX */
		&scuba_nitrox_layout_v1v2,
		/* FREEDIVE */
		&freedive_layout_v1v2v3,
		/* GAUGE */
		&gauge_layout_v1v2v3,
		/* Undefined */
		NULL,
		/* Undefined */
		NULL,
	},
	{
		/* SCUBA */
		&scuba_nitrox_layout_v1v2,
		/* NITROX */
		&scuba_nitrox_layout_v1v2,
		/* FREEDIVE */
		&freedive_layout_v1v2v3,
		/* GAUGE */
		&gauge_layout_v1v2v3,
		/* Undefined */
		NULL,
		/* Undefined */
		NULL,
	},
	{
		/* SCUBA */
		&scuba_nitrox_layout_v3,
		/* NITROX */
		&scuba_nitrox_layout_v3,
		/* FREEDIVE */
		&freedive_layout_v1v2v3,
		/* GAUGE */
		&gauge_layout_v1v2v3,
		/* Undefined */
		NULL,
		/* Undefined */
		NULL,
	},
	{
		/* SCUBA */
		&scuba_nitrox_layout_v4,
		/* NITROX */
		&scuba_nitrox_layout_v4,
		/* FREEDIVE */
		&freedive_layout_v4,
		/* GAUGE */
		&gauge_layout_v4,
		/* Undefined */
		NULL,
		/* Advanced FREEDIVE */
		&advanced_freedive_layout_v4,
	},
};

static dc_status_t
cressi_goa_irda_api_version(dc_parser_t *abstract, unsigned int model, unsigned int firmware, unsigned int *version)
{
	if (model > 11) {
		ERROR (abstract->context, "Unknown model %d.", model);
		return DC_STATUS_UNSUPPORTED;
	}
	int version_l;
	if (firmware >= 161 && firmware <= 165) {
		version_l = 0;
	} else if (firmware >= 166 && firmware <= 169) {
		version_l = 1;
	} else if (firmware >= 170 && firmware <= 179) {
		version_l = 2;
	} else if (firmware >= 100 && firmware <= 110) {
		version_l = 3;
	} else if (firmware >= 200 && firmware <= 205) {
		version_l = 4;
	} else {
		ERROR (abstract->context, "Unknown firmware version %d.", firmware);
		return DC_STATUS_UNSUPPORTED;
	}
	const unsigned int version_support_on_model[5][11] = {
		/* 1  2  3  4  5  6  7  8  9  10  11 */
		{  1, 1, 0, 0, 0, 0, 0, 0, 0,  0,  0 }, /* API v0 */
		{  1, 1, 0, 0, 0, 0, 0, 0, 0,  0,  0 }, /* API v1 */
		{  1, 1, 0, 0, 0, 0, 0, 0, 1,  0,  0 }, /* API v2 */
		{  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1 }, /* API v3 */
		{  1, 1, 0, 1, 1, 0, 0, 0, 1,  1,  0 }, /* API v4 */
	};
	if (!version_support_on_model[version_l][model - 1]) {
		ERROR (abstract->context, "Firmware version %d of Model %d not known to have support for API v%d.",
				firmware, model, version_l);
		return DC_STATUS_UNSUPPORTED;
	}
	*version = version_l;
	return DC_STATUS_SUCCESS;
}

static dc_status_t cressi_goa_get_layout(dc_parser_t *abstract)
{
	dc_status_t status;
	cressi_goa_parser_t *parser = (cressi_goa_parser_t*)abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	const unsigned int header_len = 4u;

	if (size < header_len)
		return DC_STATUS_DATAFORMAT;

	unsigned int version;
	const cressi_goa_layout_t *layout;
	unsigned int divemode;
	unsigned int data_start;

	if (data[0] == 0xdc && data[1] == 0xdc) {
		const unsigned int id_len = data[3];
		/* data[2] contains the package format version and we currently only support version 0. */
		if (data[2] != 0x00 || size < header_len + id_len) {
			return DC_STATUS_DATAFORMAT;
		}
		const unsigned char *id_data = data + header_len;
		unsigned int model = id_data[4];
		unsigned int firmware = array_uint16_le (id_data + 5);
		if (id_len == 11) {
			version = array_uint16_le (id_data + 9);
		} else {
			status = cressi_goa_irda_api_version(abstract, model, firmware, &version);
			if (status != DC_STATUS_SUCCESS) {
				return status;
			}
		}

		const unsigned char *logbook = id_data + id_len;
		const unsigned int logbook_len = version < 4 ? 23 : 15;

		if (size < header_len + id_len + logbook_len) {
			return DC_STATUS_DATAFORMAT;
		}

		divemode = logbook[2];
		if (version > NVERSIONS || divemode > NDIVEMODES) {
			ERROR (abstract->context, "Value out-of-bounds. Version %d, Divemode %d.", version, divemode);
			return DC_STATUS_DATAFORMAT;
		}
		if (layouts[version][divemode] == NULL) {
			ERROR (abstract->context, "Version %d, Divemode %d not supported.", version, divemode);
			return DC_STATUS_UNSUPPORTED;
		}

		layout = layouts[version][divemode];
		data_start = header_len + id_len + logbook_len;
	} else {
		divemode = data[2];
		if (divemode >= C_ARRAY_SIZE(layouts_original)) {
			return DC_STATUS_DATAFORMAT;
		}
		layout = &layouts_original[divemode];
		data_start = 0;
		version = 0;
	}

	if (size < layout->headersize + data_start)
		return DC_STATUS_DATAFORMAT;

	parser->layout = layout;
	parser->data_start = data_start;
	parser->divemode = divemode;
	parser->version = version;

	return DC_STATUS_SUCCESS;
}

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

	dc_status_t status = cressi_goa_get_layout((dc_parser_t*) parser);
	if (status != DC_STATUS_SUCCESS)
		goto error_free;

	parser->model = model;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
error_free:
	dc_parser_deallocate ((dc_parser_t *) parser);
	return status;
}

static dc_status_t
cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	cressi_goa_parser_t *parser = (cressi_goa_parser_t*)abstract;

	const unsigned char *p = abstract->data + parser->data_start + parser->layout->datetime;

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
	cressi_goa_parser_t *parser = (cressi_goa_parser_t*)abstract;
	const cressi_goa_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data + parser->data_start;

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
			switch (parser->divemode) {
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
	cressi_goa_parser_t *parser = (cressi_goa_parser_t*)abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int interval = parser->divemode == FREEDIVE ? 2000 : 5000;

	unsigned int offset = parser->data_start + parser->layout->headersize;
	const unsigned char *header = data + parser->data_start;

	if (parser->version == 4) {
		unsigned int sample_rate = header[parser->layout->samplerate];
		/* Valid sample rates are: 0.5s, 1s, 2s, 5s */
		unsigned int sample_rates[] = {
				500, 1000, 2000, 5000
		};
		if (sample_rate == 0 || sample_rate > sizeof(sample_rates)) {
			ERROR (abstract->context, "Unknown sample rate: 0x%02x.", sample_rate);
			return DC_STATUS_DATAFORMAT;
		}
		interval = sample_rates[sample_rate - 1];
	}

	/* Adjust the size to the number of samples reported by the DC.
	 *
	 * In Advanced Freedive mode, there are Advanced Freedive Dip Stats attached at the end.
	 * Skip those for now, but if we'd ever want to use them, their structure is:
	 *
	 * SURFTIME | MAXDEPTH | DIPTIME | MINTEMP | SPEED_DESC | SPEED_ASC | TARAVANA_VIOLATION
	 *   u16    |   u16    |   u16   |   u16   |    u16     |    u16    |       bool
	 */
	unsigned int num_samples = array_uint16_le (header + parser->layout->samplenumber);
	size = offset + num_samples * 2;

	unsigned int time = 0;
	unsigned int depth = 0;
	unsigned int depth_mask = ((parser->version < 4) ? 0x07FF : 0x0FFF);
	unsigned int gasmix = 0, gasmix_previous = 0xFFFFFFFF;
	unsigned int gasmix_mask = ((parser->version < 3) ? 0x0800 : 0x1800);
	unsigned int temperature = 0;
	unsigned int have_temperature = 0;
	unsigned int complete = 0;

	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;

		if (type == DEPTH_SCUBA) {
			depth = value & 0x7ffu;
			gasmix = (value & gasmix_mask) >> 13;
			time += interval;
			complete = 1;
		} else if (type == DEPTH_FREE) {
			depth = value & depth_mask;
			time += interval;
			complete = 1;
		} else if (type == TEMPERATURE) {
			temperature = value;
			have_temperature = 1;
		} else if (type == SURFACE) {
			// SURFACE values are not given in the sample rate, but as seconds
			unsigned int surftime = value * 1000;
			if (surftime > interval) {
				surftime -= interval;
				time += interval;

				// Time (seconds).
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
				// Depth (1/10 m).
				sample.depth = 0.0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			}
			time += surftime;
			depth = 0;
			complete = 1;
		}

		if (complete) {
			// Time (seconds).
			sample.time = time;
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
			if (parser->divemode == SCUBA || parser->divemode == NITROX) {
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
