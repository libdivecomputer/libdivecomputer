/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
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

#include <libdivecomputer/mares_nemo.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define NEMO        0
#define NEMOWIDE    1
#define NEMOAIR     4
#define PUCK        7
#define NEMOEXCEL   17
#define NEMOAPNEIST 18
#define PUCKAIR     19

typedef struct mares_nemo_parser_t mares_nemo_parser_t;

struct mares_nemo_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int freedive;
	/* Internal state */
	unsigned int mode;
	unsigned int length;
	unsigned int sample_count;
	unsigned int sample_size;
	unsigned int header;
	unsigned int extra;
};

static dc_status_t mares_nemo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t mares_nemo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_nemo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_nemo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t mares_nemo_parser_destroy (dc_parser_t *abstract);

static const parser_backend_t mares_nemo_parser_backend = {
	DC_FAMILY_MARES_NEMO,
	mares_nemo_parser_set_data, /* set_data */
	mares_nemo_parser_get_datetime, /* datetime */
	mares_nemo_parser_get_field, /* fields */
	mares_nemo_parser_samples_foreach, /* samples_foreach */
	mares_nemo_parser_destroy /* destroy */
};


static int
parser_is_mares_nemo (dc_parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_nemo_parser_backend;
}


dc_status_t
mares_nemo_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) malloc (sizeof (mares_nemo_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &mares_nemo_parser_backend);

	// Get the freedive mode for this model.
	unsigned int freedive = 2;
	if (model == NEMOWIDE || model == PUCK || model == PUCKAIR)
		freedive = 3;

	// Set the default values.
	parser->model = model;
	parser->freedive = freedive;
	parser->mode = 0;
	parser->length = 0;
	parser->sample_count = 0;
	parser->sample_size = 0;
	parser->header = 0;
	parser->extra = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_parser_destroy (dc_parser_t *abstract)
{
	if (! parser_is_mares_nemo (abstract))
		return DC_STATUS_INVALIDARGS;

	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) abstract;

	// Clear the previous state.
	parser->base.data = NULL;
	parser->base.size = 0;
	parser->mode = 0;
	parser->length = 0;
	parser->sample_count = 0;
	parser->sample_size = 0;
	parser->header = 0;
	parser->extra = 0;

	if (size == 0)
		return DC_STATUS_SUCCESS;

	if (size < 2 + 3)
		return DC_STATUS_DATAFORMAT;

	unsigned int length = array_uint16_le (data);
	if (length > size)
		return DC_STATUS_DATAFORMAT;

	unsigned int extra = 0;
	const unsigned char marker[3] = {0xAA, 0xBB, 0xCC};
	if (memcmp (data + length - 3, marker, sizeof (marker)) == 0) {
		if (parser->model == PUCKAIR)
			extra = 7;
		else
			extra = 12;
	}

	if (length < 2 + extra + 3)
		return DC_STATUS_DATAFORMAT;

	unsigned int mode = data[length - extra - 1];

	unsigned int header_size = 53;
	unsigned int sample_size = 2;
	if (extra) {
		if (parser->model == PUCKAIR)
			sample_size = 3;
		else
			sample_size = 5;
	}
	if (mode == parser->freedive) {
		header_size = 28;
		sample_size = 6;
	}

	unsigned int nsamples = array_uint16_le (data + length - extra - 3);

	unsigned int nbytes = 2 + nsamples * sample_size + header_size + extra;
	if (length != nbytes)
		return DC_STATUS_DATAFORMAT;

	// Store the new state.
	parser->base.data = data;
	parser->base.size = size;
	parser->mode = mode;
	parser->length = length;
	parser->sample_count = nsamples;
	parser->sample_size = sample_size;
	parser->header = header_size;
	parser->extra = extra;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) abstract;

	if (abstract->size == 0)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + parser->length - parser->extra - 8;

	if (datetime) {
		datetime->year   = p[0] + 2000;
		datetime->month  = p[1];
		datetime->day    = p[2];
		datetime->hour   = p[3];
		datetime->minute = p[4];
		datetime->second = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) abstract;

	if (abstract->size == 0)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;
	const unsigned char *p = abstract->data + 2 + parser->sample_count * parser->sample_size;

	if (value) {
		if (parser->mode != parser->freedive) {
			dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
			switch (type) {
			case DC_FIELD_DIVETIME:
				*((unsigned int *) value) = parser->sample_count * 20;
				break;
			case DC_FIELD_MAXDEPTH:
				*((double *) value) = array_uint16_le (p + 53 - 10) / 10.0;
				break;
			case DC_FIELD_GASMIX_COUNT:
				if (parser->mode == 0 || parser->mode == 1)
					*((unsigned int *) value) = 1;
				else
					*((unsigned int *) value) = 0;
				break;
			case DC_FIELD_GASMIX:
				switch (parser->mode) {
				case 0: // Air
					gasmix->oxygen = 0.21;
					break;
				case 1: // Nitrox
					gasmix->oxygen = p[53 - 43] / 100.0;
					break;
				default:
					return DC_STATUS_UNSUPPORTED;
				}
				gasmix->helium = 0.0;
				gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
		} else {
			unsigned int divetime = 0;
			switch (type) {
			case DC_FIELD_DIVETIME:
				for (unsigned int i = 0; i < parser->sample_count; ++i) {
					unsigned int idx = 2 + parser->sample_size * i;
					divetime += data[idx + 2] + data[idx + 3] * 60;
				}
				*((unsigned int *) value) = divetime;
				break;
			case DC_FIELD_MAXDEPTH:
				*((double *) value) = array_uint16_le (p + 28 - 10) / 10.0;
				break;
			case DC_FIELD_GASMIX_COUNT:
				*((unsigned int *) value) = 0;
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) abstract;

	if (abstract->size == 0)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (parser->mode != parser->freedive) {
		unsigned int time = 0;
		for (unsigned int i = 0; i < parser->sample_count; ++i) {
			dc_sample_value_t sample = {0};

			unsigned int idx = 2 + parser->sample_size * i;
			unsigned int value = array_uint16_le (data + idx);
			unsigned int depth = value & 0x07FF;
			unsigned int ascent = (value & 0xC000) >> 14;
			unsigned int violation = (value & 0x2000) >> 13;
			unsigned int deco = (value & 0x1000) >> 12;

			// Time (seconds).
			time += 20;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Ascent rate
			if (ascent) {
				sample.event.type = SAMPLE_EVENT_ASCENT;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = ascent;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}

			// Deco violation
			if (violation) {
				sample.event.type = SAMPLE_EVENT_CEILING;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}

			// Deco stop
			if (deco) {
				sample.event.type = SAMPLE_EVENT_DECOSTOP;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}

			// Pressure (1 bar).
			if (parser->sample_size == 3) {
				sample.pressure.tank = 0;
				sample.pressure.value = data[idx + 2];
				if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
			}
		}
	} else {
		// A freedive session contains only summaries for each individual
		// freedive. The detailed profile data (if present) is stored after
		// the normal dive data. We assume a freedive has a detailed profile
		// when the buffer contains more data than the size indicated in the
		// header.
		int profiles = (size > parser->length);

		unsigned int time = 0;
		unsigned int offset = parser->length;
		for (unsigned int i = 0; i < parser->sample_count; ++i) {
			dc_sample_value_t sample = {0};

			unsigned int idx = 2 + parser->sample_size * i;
			unsigned int maxdepth = array_uint16_le (data + idx);
			unsigned int divetime = data[idx + 2] + data[idx + 3] * 60;
			unsigned int surftime = data[idx + 4] + data[idx + 5] * 60;

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			if (profiles) {
				// Get the freedive sample interval for this model.
				unsigned int interval = 4;
				if (parser->model == NEMOAPNEIST)
					interval = 1;

				// Calculate the number of samples that should be present
				// in the profile data, based on the divetime in the summary.
				unsigned int n = (divetime + interval - 1) / interval;

				// The last sample interval can be smaller than the normal
				// 4 seconds. We keep track of the maximum divetime, to be
				// able to adjust that last sample interval.
				unsigned int maxtime = time + divetime;

				// Process all depth samples. Once a zero depth sample is
				// reached, the current freedive profile is complete.
				unsigned int count = 0;
				while (offset + 2 <= size) {
					unsigned int depth = array_uint16_le (data + offset);
					offset += 2;

					if (depth == 0)
						break;

					count++;

					if (count > n)
						break;

					// Time (seconds).
					time += interval;
					if (time > maxtime)
						time = maxtime; // Adjust the last sample.
					sample.time = time;
					if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

					// Depth (1/10 m).
					sample.depth = depth / 10.0;
					if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
				}

				// Verify that the number of samples in the profile data
				// equals the predicted number of samples (from the divetime
				// in the summary entry). If both values are different, the
				// the profile data is probably incorrect.
				if (count != n) {
					ERROR (abstract->context, "Unexpected number of samples.");
					return DC_STATUS_DATAFORMAT;
				}
			} else {
				// Dive Time (seconds).
				time += divetime;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

				// Maximum Depth (1/10 m).
				sample.depth = maxdepth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
			}
		}
	}

	return DC_STATUS_SUCCESS;
}
