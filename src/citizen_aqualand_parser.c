/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Jef Driesen
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

#include "citizen_aqualand.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &citizen_aqualand_parser_vtable)

#define SZ_HEADER 32

typedef struct citizen_aqualand_parser_t {
	dc_parser_t base;
} citizen_aqualand_parser_t;

static dc_status_t citizen_aqualand_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t citizen_aqualand_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t citizen_aqualand_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t citizen_aqualand_parser_vtable = {
	sizeof(citizen_aqualand_parser_t),
	DC_FAMILY_CITIZEN_AQUALAND,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	citizen_aqualand_parser_get_datetime, /* datetime */
	citizen_aqualand_parser_get_field, /* fields */
	citizen_aqualand_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
citizen_aqualand_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	citizen_aqualand_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (citizen_aqualand_parser_t *) dc_parser_allocate (context, &citizen_aqualand_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
citizen_aqualand_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = bcd2dec(p[0x05]) * 100 + bcd2dec(p[0x06]);
		datetime->month  = bcd2dec(p[0x07]);
		datetime->day    = bcd2dec(p[0x08]);
		datetime->hour   = bcd2dec(p[0x0A]);
		datetime->minute = bcd2dec(p[0x0B]);
		datetime->second = bcd2dec(p[0x0C]);
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
citizen_aqualand_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;

	unsigned int metric = (data[0x04] == 0xA6 ? 0 : 1);
	unsigned int maxdepth = bcd2dec(data[0x12]) * 10 + ((data[0x13] >> 4) & 0x0F);
	unsigned int divetime = (data[0x16] & 0x0F) * 100 + bcd2dec(data[0x17]);

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = divetime * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (metric)
				*((double *) value) = maxdepth / 10.0;
			else
				*((double *) value) = maxdepth * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
citizen_aqualand_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	// Estimate the maximum number of samples. We calculate the number of
	// 12 bit values that fit in the available profile data, and round the
	// result upwards. The actual number of samples should always be smaller
	// due to the presence of at least two end markers.
	unsigned int maxcount = (2 * (size - SZ_HEADER) + 2) / 3;

	// Allocate storage for the processed 16 bit samples.
	unsigned short *samples = (unsigned short *) malloc(maxcount * sizeof(unsigned short));
	if (samples == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Pre-process the depth and temperature tables. The 12 bit BCD encoded
	// values are converted into an array of 16 bit values, which is much
	// more convenient to process in the second stage.
	unsigned int nsamples = 0;
	unsigned int count[2] = {0, 0};
	unsigned int offset = SZ_HEADER * 2;
	unsigned int length = size * 2;
	for (unsigned int i = 0; i < 2; ++i) {
		const unsigned int marker = (i == 0 ? 0xEF : 0xFF);

		while (offset + 3 <= length) {
			unsigned int value = 0;
			unsigned int octet  = offset / 2;
			unsigned int nibble = offset % 2;
			unsigned int hi = data[octet];
			unsigned int lo = data[octet + 1];

			// Check for the end marker.
			if (hi == marker || lo == marker) {
				offset += nibble;
				break;
			}

			// Convert 12 bit BCD to decimal.
			if (nibble) {
				value = ((hi     ) & 0x0F) * 100 +
						((lo >> 4) & 0x0F) * 10 +
						((lo     ) & 0x0F);
			} else {
				value = ((hi >> 4) & 0x0F) * 100 +
						((hi     ) & 0x0F) * 10 +
						((lo >> 4) & 0x0F);
			}

			// Store the value.
			samples[nsamples] = value;
			count[i]++;
			nsamples++;

			offset += 3;
		}

		// Verify the end marker.
		if (offset + 2 > length || data[offset / 2] != marker) {
			ERROR (abstract->context, "No end marker found.");
			free(samples);
			return DC_STATUS_DATAFORMAT;
		}

		offset += 2;
	}

	unsigned int time = 0;
	unsigned int interval = 5;
	unsigned int metric = (data[0x04] == 0xA6 ? 0 : 1);
	for (unsigned int i = 0; i < count[0]; ++i) {
		dc_sample_value_t sample = {0};

		// Get the depth value.
		unsigned int depth = samples[i];

		// Every 12th sample there is a strange sample that always contains
		// the value 999. This is clearly not a valid depth, but when trying
		// to skip these samples, the depth and temperatures go out of sync.
		// Therefore we replace the bogus sample with an interpolated value.
		if (depth == 999) {
			depth = 0;
			if (i > 0) {
				depth += samples[i - 1];
			}
			if (i < count[0] - 1) {
				depth += samples[i + 1];
			}
			depth /= 2;
		}

		// Time
		time += interval;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth
		if (metric)
			sample.depth = depth / 10.0;
		else
			sample.depth = depth * FEET;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Temperature
		if (time % 300 == 0) {
			unsigned int idx = count[0] + time / 300;
			if (idx < nsamples) {
				unsigned int temperature = samples[idx];
				if (metric)
					sample.temperature = temperature / 10.0;
				else
					sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}
		}
	}

	free(samples);

	return DC_STATUS_SUCCESS;
}
