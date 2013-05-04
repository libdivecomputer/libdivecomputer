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

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &hw_ostc_parser_vtable)

#define MAXCONFIG 7
#define MAXGASMIX 5

typedef struct hw_ostc_parser_t hw_ostc_parser_t;

struct hw_ostc_parser_t {
	dc_parser_t base;
	unsigned int frog;
};

typedef struct hw_ostc_sample_info_t {
	unsigned int type;
	unsigned int divisor;
	unsigned int size;
} hw_ostc_sample_info_t;

typedef struct hw_ostc_layout_t {
	unsigned int datetime;
	unsigned int maxdepth;
	unsigned int divetime;
	unsigned int atmospheric;
	unsigned int salinity;
	unsigned int duration;
} hw_ostc_layout_t;

typedef struct hw_ostc_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} hw_ostc_gasmix_t;

static dc_status_t hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t hw_ostc_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t hw_ostc_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t hw_ostc_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t hw_ostc_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t hw_ostc_parser_vtable = {
	DC_FAMILY_HW_OSTC,
	hw_ostc_parser_set_data, /* set_data */
	hw_ostc_parser_get_datetime, /* datetime */
	hw_ostc_parser_get_field, /* fields */
	hw_ostc_parser_samples_foreach, /* samples_foreach */
	hw_ostc_parser_destroy /* destroy */
};

static const hw_ostc_layout_t hw_ostc_layout_ostc = {
	3,  /* datetime */
	8,  /* maxdepth */
	10, /* divetime */
	15, /* atmospheric */
	43, /* salinity */
	47, /* duration */
};

static const hw_ostc_layout_t hw_ostc_layout_frog = {
	9,  /* datetime */
	14, /* maxdepth */
	16, /* divetime */
	21, /* atmospheric */
	43, /* salinity */
	47, /* duration */
};

static const hw_ostc_layout_t hw_ostc_layout_ostc3 = {
	12, /* datetime */
	17, /* maxdepth */
	19, /* divetime */
	24, /* atmospheric */
	70, /* salinity */
	75, /* duration */
};

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
	parser_init (&parser->base, context, &hw_ostc_parser_vtable);

	parser->frog = frog;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
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
	const hw_ostc_layout_t *layout = NULL;
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		layout = &hw_ostc_layout_ostc;
		header = 47;
		break;
	case 0x21:
		layout = &hw_ostc_layout_ostc;
		header = 57;
		break;
	case 0x22:
		layout = &hw_ostc_layout_frog;
		header = 256;
		break;
	case 0x23:
		layout = &hw_ostc_layout_ostc3;
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	unsigned int divetime = 0;
	if (version > 0x20) {
		// Use the dive time stored in the extended header, rounded down towards
		// the nearest minute, to match the value displayed by the ostc.
		divetime = (array_uint16_le (data + layout->duration) / 60) * 60;
	} else {
		// Use the normal dive time (excluding the shallow parts of the dive).
		divetime = array_uint16_le (data + layout->divetime) * 60 + data[layout->divetime + 2];
	}

	const unsigned char *p = data + layout->datetime;

	dc_datetime_t dt;
	if (version == 0x23) {
		dt.year   = p[0] + 2000;
		dt.month  = p[1];
		dt.day    = p[2];
	} else {
		dt.year   = p[2] + 2000;
		dt.month  = p[0];
		dt.day    = p[1];
	}
	dt.hour   = p[3];
	dt.minute = p[4];
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
	const hw_ostc_layout_t *layout = NULL;
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		layout = &hw_ostc_layout_ostc;
		header = 47;
		break;
	case 0x21:
		layout = &hw_ostc_layout_ostc;
		header = 57;
		break;
	case 0x22:
		layout = &hw_ostc_layout_frog;
		header = 256;
		break;
	case 0x23:
		layout = &hw_ostc_layout_ostc3;
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	unsigned int salinity = data[layout->salinity];
	if (version == 0x23)
		salinity += 100;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + layout->divetime) * 60 + data[layout->divetime + 2];
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + layout->maxdepth) / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			if (version == 0x22) {
				*((unsigned int *) value) = 3;
			} else if (version == 0x23) {
				*((unsigned int *) value) = 5;
			} else {
				*((unsigned int *) value) = 6;
			}
			break;
		case DC_FIELD_GASMIX:
			if (version == 0x22) {
				gasmix->oxygen = data[25 + 2 * flags] / 100.0;
				gasmix->helium = 0.0;
			} else if (version == 0x23) {
				gasmix->oxygen = data[28 + 4 * flags + 0] / 100.0;
				gasmix->helium = data[28 + 4 * flags + 1] / 100.0;
			} else {
				gasmix->oxygen = data[19 + 2 * flags + 0] / 100.0;
				gasmix->helium = data[19 + 2 * flags + 1] / 100.0;
			}
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
			*((double *) value) = array_uint16_le (data + layout->atmospheric) / 1000.0;
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
	const hw_ostc_layout_t *layout = NULL;
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		layout = &hw_ostc_layout_ostc;
		header = 47;
		break;
	case 0x21:
		layout = &hw_ostc_layout_ostc;
		header = 57;
		break;
	case 0x22:
		layout = &hw_ostc_layout_frog;
		header = 256;
		break;
	case 0x23:
		layout = &hw_ostc_layout_ostc3;
		header = 256;
		break;
	default:
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header)
		return DC_STATUS_DATAFORMAT;

	// Get the sample rate.
	unsigned int samplerate = 0;
	if (version == 0x23)
		samplerate = data[header + 3];
	else
		samplerate = data[36];

	// Get the salinity factor.
	unsigned int salinity = data[layout->salinity];
	if (version == 0x23)
		salinity += 100;
	if (salinity < 100 || salinity > 104)
		salinity = 100;
	double hydrostatic = GRAVITY * salinity * 10.0;

	// Get all the gas mixes, and the index of the inital mix.
	unsigned int ngasmix = 0, initial = 0;
	hw_ostc_gasmix_t gasmix[MAXGASMIX] = {{0}};
	if (version == 0x22) {
		ngasmix = 3;
		initial = data[31];
		for (unsigned int i = 0; i < ngasmix; ++i) {
			gasmix[i].oxygen = data[25 + 2 * i];
			gasmix[i].helium = 0;
		}
	} else if (version == 0x23) {
		ngasmix = 5;
		for (unsigned int i = 0; i < ngasmix; ++i) {
			gasmix[i].oxygen = data[28 + 4 * i + 0];
			gasmix[i].helium = data[28 + 4 * i + 1];
			// Find the first gas marked as the initial gas.
			if (!initial && data[28 + 4 * i + 3] == 1) {
				initial = i + 1; /* One based index! */
			}
		}
	} else {
		ngasmix = 5;
		initial = data[31];
		for (unsigned int i = 0; i < ngasmix; ++i) {
			gasmix[i].oxygen = data[19 + 2 * i + 0];
			gasmix[i].helium = data[19 + 2 * i + 1];
		}
	}
	if (initial < 1 || initial > ngasmix)
		return DC_STATUS_DATAFORMAT;
	initial--; /* Convert to a zero based index. */

	// Get the number of sample descriptors.
	unsigned int nconfig = 0;
	if (version == 0x23)
		nconfig = data[header + 4];
	else
		nconfig = 6;
	if (nconfig > MAXCONFIG)
		return DC_STATUS_DATAFORMAT;

	// Get the extended sample configuration.
	hw_ostc_sample_info_t info[MAXCONFIG] = {{0}};
	for (unsigned int i = 0; i < nconfig; ++i) {
		if (version == 0x23) {
			info[i].type    = data[header + 5 + 3 * i + 0];
			info[i].size    = data[header + 5 + 3 * i + 1];
			info[i].divisor = data[header + 5 + 3 * i + 2];
		} else {
			info[i].type    = i;
			info[i].divisor = (data[37 + i] & 0x0F);
			info[i].size    = (data[37 + i] & 0xF0) >> 4;
		}

		if (info[i].divisor) {
			switch (info[i].type) {
			case 0: // Temperature
			case 1: // Deco / NDL
				if (info[i].size != 2)
					return DC_STATUS_DATAFORMAT;
				break;
			case 5: // CNS
				if (info[i].size != 1 && info[i].size != 2)
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
	if (version == 0x23)
		offset += 5 + 3 * nconfig;
	while (offset + 3 <= size) {
		dc_sample_value_t sample = {0};

		nsamples++;

		// Time (seconds).
		time += samplerate;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Initial gas mix.
		if (time == samplerate) {
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = gasmix[initial].oxygen | (gasmix[initial].helium << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}

		// Depth (mbar).
		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = (depth * BAR / 1000.0) / hydrostatic;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
		offset += 2;

		// Extended sample info.
		unsigned int length =  data[offset] & 0x7F;
		offset += 1;

		// Check for buffer overflows.
		if (offset + length > size)
			return DC_STATUS_DATAFORMAT;

		// Get the event byte(s).
		unsigned int nbits = 0;
		unsigned int events = 0;
		while (data[offset - 1] & 0x80) {
			if (offset + 1 > size)
				return DC_STATUS_DATAFORMAT;
			events |= data[offset] << nbits;
			nbits += 8;
			offset++;
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
			if (idx < 1 || idx > ngasmix)
				return DC_STATUS_DATAFORMAT;
			idx--; /* Convert to a zero based index. */
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = gasmix[idx].oxygen | (gasmix[idx].helium << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			offset++;
		}

		// SetPoint Change
		if ((events & 0x40) && (version == 0x23)) {
			if (offset + 1 > size)
				return DC_STATUS_DATAFORMAT;
			sample.setpoint = data[offset] / 100.0;
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
			offset++;
		}

		// Extended sample info.
		for (unsigned int i = 0; i < nconfig; ++i) {
			if (info[i].divisor && (nsamples % info[i].divisor) == 0) {
				unsigned int value = 0;
				switch (info[i].type) {
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
					if (info[i].size == 2)
						sample.cns = array_uint16_le (data + offset) / 100.0;
					else
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
		if ((events & 0x40) && (version != 0x23)) {
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
