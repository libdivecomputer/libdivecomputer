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

#define SMART      0x000010
#define SMARTAPNEA 0x010010
#define ICONHD    0x14
#define ICONHDNET 0x15

#define NGASMIXES 3

#define AIR       0
#define GAUGE     1
#define NITROX    2
#define FREEDIVE  3

typedef struct mares_iconhd_parser_t mares_iconhd_parser_t;

struct mares_iconhd_parser_t {
	dc_parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	unsigned int mode;
	unsigned int nsamples;
	unsigned int footer;
	unsigned int samplesize;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
};

static dc_status_t mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t mares_iconhd_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_iconhd_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_iconhd_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t mares_iconhd_parser_vtable = {
	sizeof(mares_iconhd_parser_t),
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_parser_set_data, /* set_data */
	mares_iconhd_parser_get_datetime, /* datetime */
	mares_iconhd_parser_get_field, /* fields */
	mares_iconhd_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
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

	unsigned int header = 0x5C;
	if (parser->model == ICONHDNET)
		header = 0x80;
	else if (parser->model == SMART)
		header = 4; // Type and number of samples only!
	else if (parser->model == SMARTAPNEA)
		header = 6; // Type and number of samples only!

	if (size < header + 4) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int length = array_uint32_le (data);
	if (length > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of samples in the profile data.
	unsigned int type = 0, nsamples = 0;
	if (parser->model == SMART || parser->model == SMARTAPNEA) {
		type     = array_uint16_le (data + length - header + 2);
		nsamples = array_uint16_le (data + length - header + 0);
	} else {
		type     = array_uint16_le (data + length - header + 0);
		nsamples = array_uint16_le (data + length - header + 2);
	}

	// Get the dive mode.
	unsigned int mode = type & 0x03;

	// Get the header and sample size.
	unsigned int headersize = 0x5C;
	unsigned int samplesize = 8;
	if (parser->model == ICONHDNET) {
		headersize = 0x80;
		samplesize = 12;
	} else if (parser->model == SMART) {
		if (mode == FREEDIVE) {
			headersize = 0x2E;
			samplesize = 6;
		} else {
			headersize = 0x5C;
			samplesize = 8;
		}
	} else if (parser->model == SMARTAPNEA) {
		headersize = 0x50;
		samplesize = 14;
	}

	// Calculate the total number of bytes for this dive.
	unsigned int nbytes = 4 + headersize + nsamples * samplesize;
	if (parser->model == ICONHDNET) {
		nbytes += (nsamples / 4) * 8;
	} else if (parser->model == SMARTAPNEA) {
		if (length < headersize) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}

		unsigned int settings = array_uint16_le (data + length - headersize + 0x1C);
		unsigned int divetime = array_uint32_le (data + length - headersize + 0x24);
		unsigned int samplerate = 1 << ((settings >> 9) & 0x03);

		nbytes += divetime * samplerate * 2;
	}
	if (length != nbytes) {
		ERROR (abstract->context, "Calculated and stored size are not equal.");
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned char *p = data + length - headersize;
	if (parser->model != SMART && parser->model != SMARTAPNEA) {
		p += 4;
	}

	// Gas mixes
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	if (mode == GAUGE || mode == FREEDIVE) {
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
			if (p[0x10 + ngasmixes * 4 + 1] & 0x80)
				break;
			oxygen[ngasmixes] = p[0x10 + ngasmixes * 4];
			ngasmixes++;
		}
	}

	// Cache the data for later use.
	parser->mode = mode;
	parser->nsamples = nsamples;
	parser->footer = length - headersize;
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
	mares_iconhd_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (mares_iconhd_parser_t *) dc_parser_allocate (context, &mares_iconhd_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->cached = 0;
	parser->mode = AIR;
	parser->nsamples = 0;
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
mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->mode = AIR;
	parser->nsamples = 0;
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

	const unsigned char *p = abstract->data + parser->footer;
	if (parser->model == SMART) {
		if (parser->mode == FREEDIVE) {
			p += 0x20;
		} else {
			p += 2;
		}
	} else if (parser->model == SMARTAPNEA) {
		p += 0x40;
	} else {
		p += 6;
	}

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
	if (parser->model != SMART && parser->model != SMARTAPNEA) {
		p += 4;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->model == SMARTAPNEA) {
				*((unsigned int *) value) = array_uint16_le (p + 0x24);
			} else if (parser->mode == FREEDIVE) {
				unsigned int divetime = 0;
				unsigned int offset = 4;
				for (unsigned int i = 0; i < parser->nsamples; ++i) {
					divetime += array_uint16_le (abstract->data + offset + 2);
					offset += parser->samplesize;
				}
				*((unsigned int *) value) = divetime;
			} else {
				*((unsigned int *) value) = parser->nsamples * 5;
			}
			break;
		case DC_FIELD_MAXDEPTH:
			if (parser->model == SMARTAPNEA)
				*((double *) value) = array_uint16_le (p + 0x3A) / 10.0;
			else if (parser->mode == FREEDIVE)
				*((double *) value) = array_uint16_le (p + 0x1A) / 10.0;
			else
				*((double *) value) = array_uint16_le (p + 0x00) / 10.0;
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
			if (parser->model == SMARTAPNEA)
				*((double *) value) = array_uint16_le (p + 0x38) / 1000.0;
			else if (parser->mode == FREEDIVE)
				*((double *) value) = array_uint16_le (p + 0x18) / 1000.0;
			else
				*((double *) value) = array_uint16_le (p + 0x22) / 8000.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			if (parser->model == SMARTAPNEA)
				*((double *) value) = (signed short) array_uint16_le (p + 0x3C) / 10.0;
			else if (parser->mode == FREEDIVE)
				*((double *) value) = (signed short) array_uint16_le (p + 0x1C) / 10.0;
			else
				*((double *) value) = (signed short) array_uint16_le (p + 0x42) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			if (parser->model == SMARTAPNEA)
				*((double *) value) = (signed short) array_uint16_le (p + 0x3E) / 10.0;
			else if (parser->mode == FREEDIVE)
				*((double *) value) = (signed short) array_uint16_le (p + 0x1E) / 10.0;
			else
				*((double *) value) = (signed short) array_uint16_le (p + 0x44) / 10.0;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->mode) {
			case AIR:
			case NITROX:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
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
	unsigned int samplerate = 1;
	if (parser->model == SMARTAPNEA) {
		unsigned int settings = array_uint16_le (data + parser->footer + 0x1C);
		samplerate = 1 << ((settings >> 9) & 0x03);
		if (samplerate > 1) {
			// The Smart Apnea supports multiple samples per second
			// (e.g. 2, 4 or 8). Since our smallest unit of time is one
			// second, we can't represent this, and the extra samples
			// will get dropped.
			WARNING(abstract->context, "Multiple samples per second are not supported!");
		}
		interval = 1;
	}

	// Previous gas mix - initialize with impossible value
	unsigned int gasmix_previous = 0xFFFFFFFF;

	unsigned int offset = 4;
	unsigned int nsamples = 0;
	while (nsamples < parser->nsamples) {
		dc_sample_value_t sample = {0};

		if (parser->model == SMARTAPNEA) {
			unsigned int maxdepth = array_uint16_le (data + offset + 0);
			unsigned int divetime = array_uint16_le (data + offset + 2);
			unsigned int surftime = array_uint16_le (data + offset + 4);

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			offset += parser->samplesize;
			nsamples++;

			for (unsigned int i = 0; i < divetime; ++i) {
				// Time (seconds).
				time += interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

				// Depth (1/10 m).
				unsigned int depth = array_uint16_le (data + offset);
				sample.depth = depth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

				offset += 2 * samplerate;
			}
		} else if (parser->mode == FREEDIVE) {
			unsigned int maxdepth = array_uint16_le (data + offset + 0);
			unsigned int divetime = array_uint16_le (data + offset + 2);
			unsigned int surftime = array_uint16_le (data + offset + 4);

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Dive Time (seconds).
			time += divetime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Maximum Depth (1/10 m).
			sample.depth = maxdepth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			offset += parser->samplesize;
			nsamples++;
		} else {
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
					sample.gasmix = gasmix;
					if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
#ifdef ENABLE_DEPRECATED
					sample.event.type = SAMPLE_EVENT_GASCHANGE;
					sample.event.time = 0;
					sample.event.value = parser->oxygen[gasmix];
					if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
#endif
					gasmix_previous = gasmix;
				}
			}

			offset += parser->samplesize;
			nsamples++;

			// Some extra data.
			if (parser->model == ICONHDNET && (nsamples % 4) == 0) {
				// Pressure (1/100 bar).
				unsigned int pressure = array_uint16_le(data + offset);
				sample.pressure.tank = 0;
				sample.pressure.value = pressure / 100.0;
				if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);

				offset += 8;
			}
		}
	}

	return DC_STATUS_SUCCESS;
}
