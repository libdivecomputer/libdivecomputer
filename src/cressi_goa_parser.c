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
#define NVERSIONS  6
#define NDIVEMODES 6

#define UNDEFINED 0xFFFFFFFF

typedef struct cressi_goa_parser_t cressi_goa_parser_t;

typedef struct cressi_goa_layout_t {
	unsigned int headersize;
	unsigned int nsamples;
	unsigned int samplerate;
	unsigned int datetime;
	unsigned int divetime;
	unsigned int gasmix[NGASMIXES];
	unsigned int atmospheric;
	unsigned int maxdepth;
	unsigned int avgdepth;
	unsigned int temperature;
} cressi_goa_layout_t;

struct cressi_goa_parser_t {
	dc_parser_t base;
	const cressi_goa_layout_t *layout;
	unsigned int headersize;
	unsigned int version;
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

static const cressi_goa_layout_t scuba_nitrox_layout_v0 = {
	90, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, UNDEFINED }, /* gasmix */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
};

static const cressi_goa_layout_t scuba_nitrox_layout_v1v2 = {
	92, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, UNDEFINED }, /* gasmix */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
};

static const cressi_goa_layout_t scuba_nitrox_layout_v3 = {
	92, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ 26, 28, 87 }, /* gasmix */
	30, /* atmospheric */
	73, /* maxdepth */
	75, /* avgdepth */
	77, /* temperature */
};

static const cressi_goa_layout_t scuba_nitrox_layout_v4 = {
	82, /* headersize */
	10, /* nsamples */
	2, /* samplerate */
	4, /* datetime */
	11, /* divetime */
	{ 17, 19, 21 }, /* gasmix */
	23, /* atmospheric */
	66, /* maxdepth */
	68, /* avgdepth */
	70, /* temperature */
};

static const cressi_goa_layout_t scuba_nitrox_layout_v5 = {
	83, /* headersize */
	2, /* nsamples */
	UNDEFINED, /* samplerate */
	4, /* datetime */
	11, /* divetime */
	{ 17, 19, 21 }, /* gasmix */
	23, /* atmospheric */
	66, /* maxdepth */
	68, /* avgdepth */
	70, /* temperature */
};

static const cressi_goa_layout_t freedive_layout_v0 = {
	34, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	23, /* maxdepth */
	UNDEFINED, /* avgdepth */
	25, /* temperature */
};

static const cressi_goa_layout_t freedive_layout_v1v2v3 = {
	38, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	23, /* maxdepth */
	UNDEFINED, /* avgdepth */
	25, /* temperature */
};

static const cressi_goa_layout_t freedive_layout_v4 = {
	27, /* headersize */
	2, /* nsamples */
	10, /* samplerate */
	4, /* datetime */
	11, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	15, /* maxdepth */
	UNDEFINED, /* avgdepth */
	17, /* temperature */
};

static const cressi_goa_layout_t gauge_layout_v0 = {
	38, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	22, /* atmospheric */
	24, /* maxdepth */
	26, /* avgdepth */
	28, /* temperature */
};

static const cressi_goa_layout_t gauge_layout_v1v2v3 = {
	40, /* headersize */
	10, /* nsamples */
	UNDEFINED, /* samplerate */
	12, /* datetime */
	20, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	22, /* atmospheric */
	24, /* maxdepth */
	26, /* avgdepth */
	28, /* temperature */
};

static const cressi_goa_layout_t gauge_layout_v4 = {
	28, /* headersize */
	2, /* nsamples */
	10, /* samplerate */
	4, /* datetime */
	11, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	13, /* atmospheric */
	15, /* maxdepth */
	17, /* avgdepth */
	19, /* temperature */
};

static const cressi_goa_layout_t advanced_freedive_layout_v4 = {
	28, /* headersize */
	2, /* nsamples */
	10, /* samplerate */
	4, /* datetime */
	22, /* divetime */
	{ UNDEFINED, UNDEFINED, UNDEFINED }, /* gasmix */
	UNDEFINED, /* atmospheric */
	16, /* maxdepth */
	UNDEFINED, /* avgdepth */
	18, /* temperature */
};

static const cressi_goa_layout_t * const layouts[NVERSIONS][NDIVEMODES] = {
	{
		&scuba_nitrox_layout_v0, /* SCUBA */
		&scuba_nitrox_layout_v0, /* NITROX */
		&freedive_layout_v0, /* FREEDIVE */
		&gauge_layout_v0, /* GAUGE */
		NULL, /* UNUSED */
		NULL, /* FREEDIVE_ADV */
	},
	{
		&scuba_nitrox_layout_v1v2, /* SCUBA */
		&scuba_nitrox_layout_v1v2, /* NITROX */
		&freedive_layout_v1v2v3, /* FREEDIVE */
		&gauge_layout_v1v2v3, /* GAUGE */
		NULL, /* UNUSED */
		NULL, /* FREEDIVE_ADV */
	},
	{
		&scuba_nitrox_layout_v1v2, /* SCUBA */
		&scuba_nitrox_layout_v1v2, /* NITROX */
		&freedive_layout_v1v2v3, /* FREEDIVE */
		&gauge_layout_v1v2v3, /* GAUGE */
		NULL, /* UNUSED */
		NULL, /* FREEDIVE_ADV */
	},
	{
		&scuba_nitrox_layout_v3, /* SCUBA */
		&scuba_nitrox_layout_v3, /* NITROX */
		&freedive_layout_v1v2v3, /* FREEDIVE */
		&gauge_layout_v1v2v3, /* GAUGE */
		NULL, /* UNUSED */
		NULL, /* FREEDIVE_ADV */
	},
	{
		&scuba_nitrox_layout_v4, /* SCUBA */
		&scuba_nitrox_layout_v4, /* NITROX */
		&freedive_layout_v4, /* FREEDIVE */
		&gauge_layout_v4, /* GAUGE */
		NULL, /* UNUSED */
		&advanced_freedive_layout_v4, /* FREEDIVE_ADV */
	},
	{
		&scuba_nitrox_layout_v5, /* SCUBA */
		&scuba_nitrox_layout_v5, /* NITROX */
		&freedive_layout_v4, /* FREEDIVE */
		&gauge_layout_v4, /* GAUGE */
		NULL, /* UNUSED */
		NULL, /* FREEDIVE_ADV */
	},
};

static dc_status_t
cressi_goa_init(cressi_goa_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2) {
		ERROR (abstract->context, "Invalid dive length (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int id_len = data[0];
	unsigned int logbook_len = data[1];
	if (id_len < 9 || logbook_len < 15) {
		ERROR (abstract->context, "Invalid id or logbook length (%u %u).", id_len, logbook_len);
		return DC_STATUS_DATAFORMAT;
	}

	if (size < 2 + id_len + logbook_len) {
		ERROR (abstract->context, "Invalid dive length (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned char *id = data + 2;
	const unsigned char *logbook = data + 2 + id_len;

	// Get the data format version.
	unsigned int version = 0;
	unsigned int firmware = array_uint16_le (id + 5);
	if (id_len == 11) {
		version = array_uint16_le (id + 9);
	} else {
		if (firmware >= 161 && firmware <= 165) {
			version = 0;
		} else if (firmware >= 166 && firmware <= 169) {
			version = 1;
		} else if (firmware >= 170 && firmware <= 179) {
			version = 2;
		} else if (firmware >= 100 && firmware <= 110) {
			version = 3;
		} else if (firmware >= 200 && firmware <= 205) {
			version = 4;
		} else if (firmware >= 300) {
			version = 5;
		} else {
			ERROR (abstract->context, "Unknown firmware version (%u).", firmware);
			return DC_STATUS_DATAFORMAT;
		}
	}
	if (version >= NVERSIONS) {
		ERROR (abstract->context, "Invalid data format version (%u).", version);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the dive mode.
	unsigned int divemode = logbook[2];
	if (divemode >= NDIVEMODES) {
		ERROR (abstract->context, "Invalid dive mode (%u).", divemode);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the layout.
	const cressi_goa_layout_t *layout = layouts[version][divemode];
	if (layout == NULL) {
		ERROR (abstract->context, "Unsupported dive mode for data format version %u (%u).", version, divemode);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int headersize = 2 + id_len + logbook_len;
	if (size < headersize + layout->headersize) {
		ERROR (abstract->context, "Invalid dive length (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	parser->layout = layout;
	parser->headersize = headersize;
	parser->divemode = divemode;
	parser->version = version;

	return DC_STATUS_SUCCESS;
}

dc_status_t
cressi_goa_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cressi_goa_parser_t *) dc_parser_allocate (context, &cressi_goa_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	status = cressi_goa_init(parser);
	if (status != DC_STATUS_SUCCESS)
		goto error_free;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;

error_free:
	dc_parser_deallocate ((dc_parser_t *) parser);
	return status;
}

static dc_status_t
cressi_goa_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;

	const unsigned char *p = abstract->data + parser->headersize + parser->layout->datetime;

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
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;
	const cressi_goa_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data + parser->headersize;

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
	cressi_goa_parser_t *parser = (cressi_goa_parser_t *) abstract;
	const cressi_goa_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data + parser->headersize;
	unsigned int size = abstract->size - parser->headersize;

	unsigned int interval = parser->divemode == FREEDIVE ? 2000 : 5000;
	if (layout->samplerate != UNDEFINED) {
		const unsigned int samplerates[] = {
			500, 1000, 2000, 5000
		};
		unsigned int samplerate = data[layout->samplerate];
		if (samplerate == 0 || samplerate > C_ARRAY_SIZE(samplerates)) {
			ERROR (abstract->context, "Unknown sample rate index (%u).", samplerate);
			return DC_STATUS_DATAFORMAT;
		}
		interval = samplerates[samplerate - 1];
	}

	// In advanced freedive mode, there is an extra header present after the
	// samples containing the advanced freedive dip stats.
	unsigned int trailer = parser->divemode == FREEDIVE_ADV ? 13 : 0;

	unsigned int nsamples = array_uint16_le (data + layout->nsamples);

	unsigned int time = 0;
	unsigned int depth = 0;
	unsigned int depth_mask = ((parser->version < 4) ? 0x07FF : 0x0FFF);
	unsigned int gasmix = 0, gasmix_previous = 0xFFFFFFFF;
	unsigned int gasmix_mask = ((parser->version < 3) ? 0x0800 : 0x1800);
	unsigned int temperature = 0;
	unsigned int have_temperature = 0;
	unsigned int complete = 0;

	unsigned int offset = layout->headersize;
	for (unsigned int i = 0; i < nsamples; ++i) {
		dc_sample_value_t sample = {0};

		if (offset + 2 + trailer > size) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}

		// Get the sample type and value.
		unsigned int raw = array_uint16_le (data + offset);
		unsigned int type  = (raw & 0x0003);
		unsigned int value = (raw & 0xFFFC) >> 2;

		if (type == DEPTH_SCUBA) {
			depth = value & 0x07FF;
			gasmix = (value & gasmix_mask) >> 11;
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
