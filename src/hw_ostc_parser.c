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

#include <libdivecomputer/hw_ostc.h>
#include "libdivecomputer/units.h"

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define NINFO 6

typedef struct hw_ostc_parser_t hw_ostc_parser_t;

struct hw_ostc_parser_t {
	dc_parser_t base;
	unsigned int frog;
};

typedef struct hw_ostc_sample_info_t {
	unsigned int divisor;
	unsigned int size;
} hw_ostc_sample_info_t;

static dc_status_t hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t hw_ostc_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t hw_ostc_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t hw_ostc_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t hw_ostc_parser_destroy (dc_parser_t *abstract);

static const parser_backend_t hw_ostc_parser_backend = {
	DC_FAMILY_HW_OSTC,
	hw_ostc_parser_set_data, /* set_data */
	hw_ostc_parser_get_datetime, /* datetime */
	hw_ostc_parser_get_field, /* fields */
	hw_ostc_parser_samples_foreach, /* samples_foreach */
	hw_ostc_parser_destroy /* destroy */
};


static int
parser_is_hw_ostc (dc_parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &hw_ostc_parser_backend;
}


dc_status_t
hw_ostc_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int frog)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) malloc (sizeof (hw_ostc_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &hw_ostc_parser_backend);

	parser->frog = frog;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_destroy (dc_parser_t *abstract)
{
	if (! parser_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 9)
		return DC_STATUS_DATAFORMAT;

	// Check the profile version
	unsigned int version = data[parser->frog ? 8 : 2];
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		header = 47;
		break;
	case 0x21:
		header = 57;
		break;
	case 0x22:
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	unsigned int divetime = 0;
	if (version == 0x21 || version == 0x22) {
		// Use the dive time stored in the extended header, rounded down towards
		// the nearest minute, to match the value displayed by the ostc.
		divetime = (array_uint16_le (data + 47) / 60) * 60;
	} else {
		// Use the normal dive time (excluding the shallow parts of the dive).
		divetime = array_uint16_le (data + 10) * 60 + data[12];
	}

	if (parser->frog)
		data += 6;

	dc_datetime_t dt;
	dt.year   = data[5] + 2000;
	dt.month  = data[3];
	dt.day    = data[4];
	dt.hour   = data[6];
	dt.minute = data[7];
	dt.second = 0;

	dc_ticks_t ticks = dc_datetime_mktime (&dt);
	if (ticks == (dc_ticks_t) -1)
		return DC_STATUS_DATAFORMAT;

	ticks -= divetime;

	if (!dc_datetime_localtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 9)
		return DC_STATUS_DATAFORMAT;

	// Check the profile version
	unsigned int version = data[parser->frog ? 8 : 2];
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		header = 47;
		break;
	case 0x21:
		header = 57;
		break;
	case 0x22:
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	if (parser->frog)
		data += 6;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	unsigned int salinity = data[43];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + 10) * 60 + data[12];
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 8) / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			if (parser->frog)
				*((unsigned int *) value) = 3;
			else
				*((unsigned int *) value) = 6;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = data[19 + 2 * flags] / 100.0;
			if (parser->frog)
				gasmix->helium = 0.0;
			else
				gasmix->helium = data[20 + 2 * flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			if (salinity < 100 || salinity > 104)
				return DC_STATUS_UNSUPPORTED;

			if (salinity == 100)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = salinity * 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_le (data + 15) / 1000.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 9)
		return DC_STATUS_DATAFORMAT;

	// Check the profile version
	unsigned int version = data[parser->frog ? 8 : 2];
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		header = 47;
		break;
	case 0x21:
		header = 57;
		break;
	case 0x22:
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	// Get the sample rate.
	unsigned int samplerate = data[36];

	// Get the salinity factor.
	unsigned int salinity = data[43];
	if (salinity < 100 || salinity > 104)
		salinity = 100;
	double hydrostatic = GRAVITY * salinity * 10.0;

	// Get the extended sample configuration.
	hw_ostc_sample_info_t info[NINFO];
	for (unsigned int i = 0; i < NINFO; ++i) {
		info[i].divisor = (data[37 + i] & 0x0F);
		info[i].size    = (data[37 + i] & 0xF0) >> 4;
		if (info[i].divisor) {
			switch (i) {
			case 0: // Temperature
			case 1: // Deco / NDL
				if (info[i].size != 2)
					return DC_STATUS_DATAFORMAT;
				break;
			case 5: // CNS
				if (info[i].size != 1)
					return DC_STATUS_DATAFORMAT;
				break;
			default: // Not yet used.
				break;
			}
		}
	}

	unsigned int time = 0;
	unsigned int nsamples = 0;

	unsigned int offset = header;
	while (offset + 3 <= size) {
		dc_sample_value_t sample = {0};

		nsamples++;

		// Time (seconds).
		time += samplerate;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Initial gas mix.
		if (time == samplerate) {
			unsigned int idx = data[31];
			if (idx < 1 || idx > 5)
				return DC_STATUS_DATAFORMAT;
			idx--; /* Convert to a zero based index. */
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = data[19 + 2 * idx] | (data[20 + 2 * idx] << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}

		// Depth (mbar).
		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = (depth * BAR / 1000.0) / hydrostatic;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
		offset += 2;

		// Extended sample info.
		unsigned int length =  data[offset] & 0x7F;
		unsigned int events = (data[offset] & 0x80) >> 7;
		offset += 1;

		// Check for buffer overflows.
		if (offset + length > size)
			return DC_STATUS_DATAFORMAT;

		// Get the event byte.
		if (events) {
			events = data[offset++];
		}

		// Alarms
		sample.event.type = 0;
		sample.event.time = 0;
		sample.event.flags = 0;
		sample.event.value = 0;
		switch (events & 0x0F) {
		case 0: // No Alarm
			break;
		case 1: // Slow
			sample.event.type = SAMPLE_EVENT_ASCENT;
			break;
		case 2: // Deco Stop missed
			sample.event.type = SAMPLE_EVENT_CEILING;
			break;
		case 3: // Deep Stop missed
			sample.event.type = SAMPLE_EVENT_CEILING;
			break;
		case 4: // ppO2 Low Warning
			sample.event.type = SAMPLE_EVENT_PO2;
			break;
		case 5: // ppO2 High Warning
			sample.event.type = SAMPLE_EVENT_PO2;
			break;
		case 6: // Manual Marker
			sample.event.type = SAMPLE_EVENT_BOOKMARK;
			break;
		case 7: // Low Battery
			break;
		}
		if (sample.event.type && callback)
			callback (DC_SAMPLE_EVENT, sample, userdata);

		// Manual Gas Set & Change
		if (events & 0x10) {
			if (offset + 2 > size)
				return DC_STATUS_DATAFORMAT;
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = data[offset] | (data[offset + 1] << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			offset += 2;
		}

		// Gas Change
		if (events & 0x20) {
			if (offset + 1 > size)
				return DC_STATUS_DATAFORMAT;
			unsigned int idx = data[offset];
			if (idx < 1 || idx > 5)
				return DC_STATUS_DATAFORMAT;
			idx--; /* Convert to a zero based index. */
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = data[19 + 2 * idx] | (data[20 + 2 * idx] << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			offset++;
		}

		// Extended sample info.
		for (unsigned int i = 0; i < NINFO; ++i) {
			if (info[i].divisor && (nsamples % info[i].divisor) == 0) {
				unsigned int value = 0;
				switch (i) {
				case 0: // Temperature (0.1 °C).
					value = array_uint16_le (data + offset);
					sample.temperature = value / 10.0;
					if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
					break;
				case 1: // Deco / NDL
					if (data[offset]) {
						sample.deco.type = DC_DECO_DECOSTOP;
						sample.deco.depth = data[offset];
					} else {
						sample.deco.type = DC_DECO_NDL;
						sample.deco.depth = 0.0;
					}
					sample.deco.time = data[offset + 1] * 60;
					if (callback) callback (DC_SAMPLE_DECO, sample, userdata);
					break;
				case 5: // CNS
					sample.cns = data[offset] / 100.0;
					if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
					break;
				default: // Not yet used.
					break;
				}

				offset += info[i].size;
			}
		}

		// SetPoint Change
		if (events & 0x40) {
			if (offset + 1 > size)
				return DC_STATUS_DATAFORMAT;
			sample.setpoint = data[offset] / 100.0;
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
			offset++;
		}
	}

	if (data[offset] != 0xFD || data[offset + 1] != 0xFD)
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}
