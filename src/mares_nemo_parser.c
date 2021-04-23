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

#include <libdivecomputer/units.h>

#include "mares_nemo.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &mares_nemo_parser_vtable)

#define NEMO        0
#define NEMOWIDE    1
#define NEMOAIR     4
#define PUCK        7
#define NEMOEXCEL   17
#define NEMOAPNEIST 18
#define PUCKAIR     19

#define AIR      0
#define NITROX   1
#define FREEDIVE 2
#define GAUGE    3

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

static dc_status_t mares_nemo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_nemo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_nemo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t mares_nemo_parser_vtable = {
	sizeof(mares_nemo_parser_t),
	DC_FAMILY_MARES_NEMO,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	mares_nemo_parser_get_datetime, /* datetime */
	mares_nemo_parser_get_field, /* fields */
	mares_nemo_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
mares_nemo_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_nemo_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (mares_nemo_parser_t *) dc_parser_allocate (context, &mares_nemo_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Get the freedive mode for this model.
	unsigned int freedive = FREEDIVE;
	if (model == NEMOWIDE || model == NEMOAIR || model == PUCK || model == PUCKAIR)
		freedive = GAUGE;

	if (size < 2 + 3) {
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	unsigned int length = array_uint16_le (data);
	if (length > size) {
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	unsigned int extra = 0;
	const unsigned char marker[3] = {0xAA, 0xBB, 0xCC};
	if (memcmp (data + length - 3, marker, sizeof (marker)) == 0) {
		if (model == PUCKAIR)
			extra = 7;
		else
			extra = 12;
	}

	if (length < 2 + extra + 3) {
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	unsigned int mode = data[length - extra - 1];

	unsigned int header_size = 53;
	unsigned int sample_size = 2;
	if (extra) {
		if (model == PUCKAIR)
			sample_size = 3;
		else
			sample_size = 5;
	}
	if (mode == freedive) {
		header_size = 28;
		sample_size = 6;
	}

	unsigned int nsamples = array_uint16_le (data + length - extra - 3);

	unsigned int nbytes = 2 + nsamples * sample_size + header_size + extra;
	if (length != nbytes) {
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	// Set the default values.
	parser->model = model;
	parser->freedive = freedive;
	parser->mode = mode;
	parser->length = length;
	parser->sample_count = nsamples;
	parser->sample_size = sample_size;
	parser->header = header_size;
	parser->extra = extra;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;

error_free:
	dc_parser_deallocate ((dc_parser_t *) parser);
	return status;
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
		datetime->timezone = DC_TIMEZONE_NONE;
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
			dc_tank_t *tank = (dc_tank_t *) value;
			switch (type) {
			case DC_FIELD_DIVETIME:
				*((unsigned int *) value) = parser->sample_count * 20;
				break;
			case DC_FIELD_MAXDEPTH:
				*((double *) value) = array_uint16_le (p + 53 - 10) / 10.0;
				break;
			case DC_FIELD_GASMIX_COUNT:
				if (parser->mode == AIR || parser->mode == NITROX)
					*((unsigned int *) value) = 1;
				else
					*((unsigned int *) value) = 0;
				break;
			case DC_FIELD_GASMIX:
				switch (parser->mode) {
				case AIR:
					gasmix->oxygen = 0.21;
					break;
				case NITROX:
					gasmix->oxygen = p[53 - 43] / 100.0;
					break;
				default:
					return DC_STATUS_UNSUPPORTED;
				}
				gasmix->helium = 0.0;
				gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
				gasmix->usage = DC_USAGE_NONE;
				break;
			case DC_FIELD_TANK_COUNT:
				if (parser->extra)
					*((unsigned int *) value) = 1;
				else
					*((unsigned int *) value) = 0;
				break;
			case DC_FIELD_TANK:
				if (parser->extra == 12) {
					unsigned int volume = array_uint16_le(p + parser->header + 0);
					unsigned int workpressure = array_uint16_le(p + parser->header + 2);
					if (workpressure == 0xFFFF) {
						tank->type = DC_TANKVOLUME_METRIC;
						tank->volume = volume / 10.0;
						tank->workpressure = 0.0;
					} else {
						if (workpressure == 0)
							return DC_STATUS_DATAFORMAT;
						tank->type = DC_TANKVOLUME_IMPERIAL;
						tank->volume = volume * CUFT * 1000.0;
						tank->volume /= workpressure * PSI / ATM;
						tank->workpressure = workpressure * PSI / BAR;
					}
					tank->beginpressure = array_uint16_le(p + parser->header + 4) / 100.0;
					tank->endpressure = array_uint16_le(p + parser->header + 6) / 100.0;
				} else if (parser->extra == 7) {
					tank->type = DC_TANKVOLUME_NONE;
					tank->volume = 0.0;
					tank->workpressure = 0.0;
					tank->beginpressure = array_uint16_le(p + parser->header + 0);
					tank->endpressure = array_uint16_le(p + parser->header + 2);
				} else {
					return DC_STATUS_UNSUPPORTED;
				}
				if (parser->mode == AIR || parser->mode == NITROX) {
					tank->gasmix = 0;
				} else {
					tank->gasmix = DC_GASMIX_UNKNOWN;
				}
				tank->usage = DC_USAGE_NONE;
				break;
			case DC_FIELD_TEMPERATURE_MINIMUM:
				*((double *) value) = (signed char) p[53 - 11];
				break;
			case DC_FIELD_DIVEMODE:
				switch (parser->mode) {
				case AIR:
				case NITROX:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case FREEDIVE:
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
			case DC_FIELD_TEMPERATURE_MINIMUM:
				*((double *) value) = (signed char) p[28 - 11];
				break;
			case DC_FIELD_DIVEMODE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
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
		// Initial tank pressure.
		unsigned int pressure = 0;
		if (parser->extra == 12) {
			const unsigned char *p = data + 2 + parser->sample_count * parser->sample_size;
			pressure = array_uint16_le(p + parser->header + 4);
		}

		// Initial gas mix.
		unsigned int gasmix_previous = 0xFFFFFFFF;
		unsigned int gasmix = gasmix_previous;
		if (parser->mode == AIR || parser->mode == NITROX) {
			gasmix = 0;
		}

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

			// Pressure (1 bar).
			if (parser->sample_size == 3) {
				sample.pressure.tank = 0;
				sample.pressure.value = data[idx + 2];
				if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
			} else if (parser->sample_size == 5) {
				unsigned int type = (time / 20) % 3;
				if (type == 0) {
					pressure -= data[idx + 2] * 100;
					sample.pressure.tank = 0;
					sample.pressure.value = pressure / 100.0;
					if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
				}
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
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

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
					sample.time = time * 1000;
					if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

					// Depth (1/10 m).
					sample.depth = depth / 10.0;
					if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
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
				sample.time = time * 1000;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Maximum Depth (1/10 m).
				sample.depth = maxdepth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			}
		}
	}

	return DC_STATUS_SUCCESS;
}
