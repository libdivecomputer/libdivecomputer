/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &oceanic_atom2_parser_vtable)

#define NORMAL   0
#define GAUGE    1
#define FREEDIVE 2

#define DSX_CC        0
#define DSX_OC        1
#define DSX_SIDEMOUNT 2
#define DSX_SIDEGAUGE 3
#define DSX_GAUGE     4

#define NGASMIXES 6

#define HEADER  1
#define PROFILE 2

typedef struct oceanic_atom2_parser_t oceanic_atom2_parser_t;

struct oceanic_atom2_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int logbooksize;
	unsigned int headersize;
	unsigned int footersize;
	// Cached fields.
	unsigned int cached;
	unsigned int header;
	unsigned int footer;
	unsigned int mode;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int divetime;
	double maxdepth;
};

static dc_status_t oceanic_atom2_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t oceanic_atom2_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t oceanic_atom2_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t oceanic_atom2_parser_vtable = {
	sizeof(oceanic_atom2_parser_t),
	DC_FAMILY_OCEANIC_ATOM2,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	oceanic_atom2_parser_get_datetime, /* datetime */
	oceanic_atom2_parser_get_field, /* fields */
	oceanic_atom2_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static unsigned int
is_freedive (unsigned int divemode, unsigned int model)
{
	return divemode == FREEDIVE && model != DSX;
}

dc_status_t
oceanic_atom2_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	oceanic_atom2_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (oceanic_atom2_parser_t *) dc_parser_allocate (context, &oceanic_atom2_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->logbooksize = 0;
	parser->headersize = 9 * PAGESIZE / 2;
	parser->footersize = 2 * PAGESIZE / 2;
	if (model == DATAMASK || model == COMPUMASK ||
		model == GEO || model == GEO20 ||
		model == VEO20 || model == VEO30 ||
		model == OCS || model == PROPLUS3 ||
		model == A300 || model == MANTA ||
		model == INSIGHT2 || model == ZEN ||
		model == I300 || model == I550 ||
		model == I200 || model == I200C ||
		model == I300C || model == GEO40 ||
		model == VEO40 || model == I470TC ||
		model == GEOAIR) {
		parser->headersize -= PAGESIZE;
	} else if (model == VT4 || model == VT41) {
		parser->headersize += PAGESIZE;
	} else if (model == TX1) {
		parser->headersize += 2 * PAGESIZE;
	} else if (model == ATOM1 || model == I100 ||
		model == PROPLUS4) {
		parser->headersize -= 2 * PAGESIZE;
	} else if (model == F10A || model == F10B ||
		model == MUNDIAL2 || model == MUNDIAL3) {
		parser->headersize = 3 * PAGESIZE;
		parser->footersize = 0;
	} else if (model == F11A || model == F11B) {
		parser->headersize = 5 * PAGESIZE;
		parser->footersize = 0;
	} else if (model == A300CS || model == VTX ||
		model == I450T || model == I750TC ||
		model == I770R || model == SAGE ||
		model == BEACON) {
		parser->headersize = 5 * PAGESIZE;
	} else if (model == PROPLUSX) {
		parser->headersize = 3 * PAGESIZE;
	} else if (model == I550C || model == WISDOM4 ||
		model == I200CV2|| model == I100V2) {
		parser->headersize = 5 * PAGESIZE / 2;
	} else if (model == I330R || model == I330R_C) {
		parser->logbooksize = 64;
		parser->headersize = parser->logbooksize + 80;
		parser->footersize = 48;
	} else if (model == DSX) {
		parser->logbooksize = 512;
		parser->headersize = parser->logbooksize + 256;
		parser->footersize = 64;
	}

	parser->cached = 0;
	parser->header = 0;
	parser->footer = 0;
	parser->mode = NORMAL;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->divetime = 0;
	parser->maxdepth = 0.0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	unsigned int header = 8;
	if (parser->model == F10A || parser->model == F10B ||
		parser->model == F11A || parser->model == F11B ||
		parser->model == MUNDIAL2 || parser->model == MUNDIAL3)
		header = 32;

	if (abstract->size < header)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		// AM/PM bit of the 12-hour clock.
		unsigned int pm = p[1] & 0x80;
		unsigned int have_ampm = 1;

		switch (parser->model) {
		case I330R:
		case I330R_C:
		case DSX:
			datetime->year   = p[7] + 2000;
			datetime->month  = p[6];
			datetime->day    = p[5];
			datetime->hour   = p[3];
			datetime->minute = p[4];
			have_ampm = 0;
			break;
		case OC1A:
		case OC1B:
		case OC1C:
		case OCS:
		case VT4:
		case VT41:
		case ATOM3:
		case ATOM31:
		case A300AI:
		case OCI:
		case I550:
		case I550C:
		case VISION:
		case XPAIR:
		case WISDOM4:
		case I470TC:
		case I200CV2:
		case GEOAIR:
		case I100V2:
			datetime->year   = ((p[5] & 0xE0) >> 5) + ((p[7] & 0xE0) >> 2) + 2000;
			datetime->month  = (p[3] & 0x0F);
			datetime->day    = ((p[0] & 0x80) >> 3) + ((p[3] & 0xF0) >> 4);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0] & 0x7F);
			break;
		case VT3:
		case VEO20:
		case VEO30:
		case DG03:
		case T3A:
		case T3B:
		case GEO20:
		case PROPLUS3:
		case DATAMASK:
		case COMPUMASK:
		case INSIGHT2:
		case I300:
		case I200:
		case I200C:
		case I100:
		case I300C:
		case GEO40:
		case VEO40:
		case PROPLUS4:
			datetime->year   = ((p[3] & 0xE0) >> 1) + (p[4] & 0x0F) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			datetime->day    = p[3] & 0x1F;
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0]);
			break;
		case ZENAIR:
		case AMPHOS:
		case AMPHOSAIR:
		case VOYAGER2G:
		case TALIS:
		case AMPHOS2:
		case AMPHOSAIR2:
			datetime->year   = (p[3] & 0x1F) + 2000;
			datetime->month  = (p[7] & 0xF0) >> 4;
			datetime->day    = ((p[3] & 0x80) >> 3) + ((p[5] & 0xF0) >> 4);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0]);
			break;
		case F10A:
		case F10B:
		case F11A:
		case F11B:
		case MUNDIAL2:
		case MUNDIAL3:
			datetime->year   = bcd2dec (p[6]) + 2000;
			datetime->month  = bcd2dec (p[7]);
			datetime->day    = bcd2dec (p[8]);
			datetime->hour   = bcd2dec (p[13] & 0x7F);
			datetime->minute = bcd2dec (p[12]);
			pm = p[13] & 0x80;
			break;
		case TX1:
			datetime->year   = bcd2dec (p[13]) + 2000;
			datetime->month  = bcd2dec (p[14]);
			datetime->day    = bcd2dec (p[15]);
			datetime->hour   = p[11];
			datetime->minute = p[10];
			break;
		case A300CS:
		case VTX:
		case I450T:
		case I750TC:
		case PROPLUSX:
		case I770R:
		case SAGE:
		case BEACON:
			datetime->year   = (p[10]) + 2000;
			datetime->month  = (p[8]);
			datetime->day    = (p[9]);
			datetime->hour   = bcd2dec(p[1] & 0x1F);
			datetime->minute = bcd2dec(p[0]);
			break;
		default:
			datetime->year   = bcd2dec (((p[3] & 0xC0) >> 2) + (p[4] & 0x0F)) + 2000;
			datetime->month  = (p[4] & 0xF0) >> 4;
			datetime->day    = bcd2dec (p[3] & 0x3F);
			datetime->hour   = bcd2dec (p[1] & 0x1F);
			datetime->minute = bcd2dec (p[0]);
			break;
		}
		datetime->second = 0;
		datetime->timezone = DC_TIMEZONE_NONE;

		// Convert to a 24-hour clock.
		if (have_ampm) {
			datetime->hour %= 12;
			if (pm)
				datetime->hour += 12;
		}

		/*
		 * Workaround for the year 2010 problem.
		 *
		 * In theory there are more than enough bits available to store years
		 * past 2010. Unfortunately some models do not use all those bits and
		 * store only the last digit of the year. We try to guess the missing
		 * information based on the current year. This should work in most
		 * cases, except when the dive is more than 10 years old or in the
		 * future (due to an incorrect clock on the device or the host system).
		 *
		 * Note that we are careful not to apply any guessing when the year is
		 * actually stored with more bits. We don't want the code to break when
		 * a firmware update fixes this bug.
		 */

		if (datetime->year < 2010) {
			// Retrieve the current year.
			dc_datetime_t now = {0};
			if (dc_datetime_localtime (&now, dc_datetime_now ()) &&
				now.year >= 2010)
			{
				// Guess the correct decade.
				int decade = (now.year / 10) * 10;
				if (datetime->year % 10 > now.year % 10)
					decade -= 10; /* Force back to the previous decade. */

				// Adjust the year.
				datetime->year += decade - 2000;
			}
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_parser_cache (oceanic_atom2_parser_t *parser)
{
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	// Get the total amount of bytes before and after the profile data.
	unsigned int headersize = parser->headersize;
	unsigned int footersize = parser->footersize;
	if (size < headersize + footersize)
		return DC_STATUS_DATAFORMAT;

	// Get the offset to the header and footer sample.
	unsigned int header = headersize - PAGESIZE / 2;
	unsigned int footer = size - footersize;
	if (parser->model == VT4 || parser->model == VT41 ||
		parser->model == A300AI || parser->model == VISION ||
		parser->model == XPAIR) {
		header = 3 * PAGESIZE;
	}

	// Get the dive mode.
	unsigned int mode = NORMAL;
	if (parser->model == F10A || parser->model == F10B ||
		parser->model == F11A || parser->model == F11B ||
		parser->model == MUNDIAL2 || parser->model == MUNDIAL3) {
		mode = FREEDIVE;
	} else if (parser->model == T3B || parser->model == VT3 ||
		parser->model == DG03) {
		mode = (data[2] & 0xC0) >> 6;
	} else if (parser->model == VEO20 || parser->model == VEO30 ||
		parser->model == OCS) {
		mode = (data[1] & 0x60) >> 5;
	} else if (parser->model == I330R || parser->model == I330R_C) {
		mode = data[2];
	} else if (parser->model == DSX) {
		mode = data[45];
	}

	// Get the gas mixes.
	unsigned int ngasmixes = 0;
	unsigned int o2_offset = 0;
	unsigned int he_offset = 0;
	unsigned int o2_step = 1;
	if (is_freedive (mode, parser->model)) {
		ngasmixes = 0;
	} else if (parser->model == DATAMASK || parser->model == COMPUMASK) {
		ngasmixes = 1;
		o2_offset = header + 3;
	} else if (parser->model == VT4 || parser->model == VT41 ||
		parser->model == A300AI || parser->model == VISION ||
		parser->model == XPAIR) {
		o2_offset = header + 4;
		ngasmixes = 4;
	} else if (parser->model == OCI) {
		o2_offset = 0x28;
		ngasmixes = 4;
	} else if (parser->model == TX1) {
		o2_offset = 0x3E;
		he_offset = 0x48;
		ngasmixes = 6;
	} else if (parser->model == A300CS || parser->model == VTX ||
		parser->model == I750TC || parser->model == SAGE ||
		parser->model == BEACON) {
		o2_offset = 0x2A;
		if (data[0x39] & 0x04) {
			ngasmixes = 1;
		} else if (data[0x39] & 0x08) {
			ngasmixes = 2;
		} else if (data[0x39] & 0x10) {
			ngasmixes = 3;
		} else {
			ngasmixes = 4;
		}
	} else if (parser->model == I450T) {
		o2_offset = 0x30;
		ngasmixes = 3;
	} else if (parser->model == ZEN) {
		o2_offset = header + 4;
		ngasmixes = 2;
	} else if (parser->model == PROPLUSX) {
		o2_offset = 0x24;
		ngasmixes = 4;
	} else if (parser->model == I770R) {
		o2_offset = 0x30;
		ngasmixes = 4;
		o2_step = 2;
	} else if (parser->model == I470TC) {
		o2_offset = 0x28;
		ngasmixes = 3;
		o2_step = 2;
	} else if (parser->model == WISDOM4) {
		o2_offset = header + 4;
		ngasmixes = 1;
	} else if (parser->model == I330R || parser->model == I330R_C) {
		ngasmixes = 3;
		o2_offset = parser->logbooksize + 16;
	} else if (parser->model == DSX) {
		if (mode < DSX_SIDEGAUGE) {
			o2_offset = parser->logbooksize + 0x89 + mode * 16;
			he_offset = parser->logbooksize + 0xB9 + mode * 16;
			ngasmixes = 6;
		} else {
			ngasmixes = 0;
		}
	} else {
		o2_offset = header + 4;
		ngasmixes = 3;
	}

	// Cache the data for later use.
	parser->header = header;
	parser->footer = footer;
	parser->mode = mode;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		if (data[o2_offset + i * o2_step]) {
			parser->oxygen[i] = data[o2_offset + i * o2_step];
			// The i330R uses 20 as "Air" and 21 as 21% Nitrox
			if ((parser->model == I330R || parser->model == I330R_C) &&
				parser->oxygen[i] == 20) {
				parser->oxygen[i] = 21;
			}
		} else {
			parser->oxygen[i] = 21;
		}
		if (he_offset) {
			parser->helium[i] = data[he_offset + i];
		} else {
			parser->helium[i] = 0;
		}
	}
	parser->cached = HEADER;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the header data.
	status = oceanic_atom2_parser_cache (parser);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// Cache the profile data.
	if (parser->cached < PROFILE) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		status = oceanic_atom2_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (status != DC_STATUS_SUCCESS)
			return status;

		parser->cached = PROFILE;
		parser->divetime = statistics.divetime;
		parser->maxdepth = statistics.maxdepth;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->model == F10A || parser->model == F10B ||
				parser->model == F11A || parser->model == F11B ||
				parser->model == MUNDIAL2 || parser->model == MUNDIAL3)
				*((unsigned int *) value) = bcd2dec (data[2]) + bcd2dec (data[3]) * 60;
			else
				*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			if (parser->model == F10A || parser->model == F10B ||
				parser->model == F11A || parser->model == F11B ||
				parser->model == MUNDIAL2 || parser->model == MUNDIAL3)
				*((double *) value) = array_uint16_le (data + 4) / 16.0 * FEET;
			else if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX)
				*((double *) value) = array_uint16_le (data + parser->footer + 10) / 10.0 * FEET;
			else
				*((double *) value) = (array_uint16_le (data + parser->footer + 4) & 0x0FFF) / 16.0 * FEET;
			break;
		case DC_FIELD_AVGDEPTH:
			if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
				*((double *) value) = array_uint16_le (data + parser->footer + 12) / 10.0 * FEET;
			} else {
				return DC_STATUS_UNSUPPORTED;
			}
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			if (parser->model == A300CS || parser->model == VTX ||
				parser->model == I750TC) {
				if (data[0x18] & 0x80) {
					water->type = DC_WATER_FRESH;
				} else {
					water->type = DC_WATER_SALT;
				}
				water->density = 0.0;
			} else if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
				unsigned int settings = array_uint32_le (data + parser->logbooksize + 12);
				if (settings & 0x10000) {
					water->type = DC_WATER_FRESH;
				} else {
					water->type = DC_WATER_SALT;
				}
				water->density = 0.0;
			} else {
				return DC_STATUS_UNSUPPORTED;
			}
			break;
		case DC_FIELD_DIVEMODE:
			if (parser->model == DSX) {
				switch (parser->mode) {
				case DSX_OC:
				case DSX_SIDEMOUNT:
					*((unsigned int *) value) = DC_DIVEMODE_OC;
					break;
				case DSX_SIDEGAUGE:
				case DSX_GAUGE:
					*((unsigned int *) value) = DC_DIVEMODE_GAUGE;
					break;
				case DSX_CC:
					*((unsigned int *) value) = DC_DIVEMODE_CCR;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else {
				switch (parser->mode) {
				case NORMAL:
					*((unsigned int *) value) = DC_DIVEMODE_OC;
					break;
				case GAUGE:
					*((unsigned int *) value) = DC_DIVEMODE_GAUGE;
					break;
				case FREEDIVE:
					*((unsigned int *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			}
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static void
oceanic_atom2_parser_vendor (oceanic_atom2_parser_t *parser, const unsigned char *data, unsigned int size, unsigned int samplesize, dc_sample_callback_t callback, void *userdata)
{
	unsigned int offset = 0;
	while (offset + samplesize <= size) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if ((!is_freedive (parser->mode, parser->model) &&
			array_isequal (data + offset, samplesize, 0x00)) ||
			array_isequal (data + offset, samplesize, 0xFF)) {
			offset += samplesize;
			continue;
		}

		// Get the sample type.
		unsigned int sampletype = data[offset + 0];
		if (is_freedive (parser->mode, parser->model))
			sampletype = 0;

		// Get the sample size.
		unsigned int length = samplesize;
		if (sampletype == 0xBB) {
			length = PAGESIZE;
		}

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_ATOM2;
		sample.vendor.size = length;
		sample.vendor.data = data + offset;
		if (callback) callback (DC_SAMPLE_VENDOR, &sample, userdata);

		offset += length;
	}
}

static dc_status_t
oceanic_atom2_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_parser_t *parser = (oceanic_atom2_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the header data.
	status = oceanic_atom2_parser_cache (parser);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned int extratime = 0;
	unsigned int time = 0;
	unsigned int interval = 1000;
	if (!is_freedive (parser->mode, parser->model)) {
		if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
			interval = data[parser->logbooksize + 36] * 1000;
		} else {
			unsigned int offset = 0x17;
			if (parser->model == A300CS || parser->model == VTX ||
				parser->model == I450T || parser->model == I750TC ||
				parser->model == PROPLUSX || parser->model == I770R ||
				parser->model == SAGE || parser->model == BEACON)
				offset = 0x1f;
			const unsigned int intervals[] = {2000, 15000, 30000, 60000};
			unsigned int idx = data[offset] & 0x03;
			interval = intervals[idx];
		}
	} else if (parser->model == F11A || parser->model == F11B) {
		const unsigned int intervals[] = {250, 500, 1000, 2000};
		unsigned int idx = data[0x29] & 0x03;
		interval = intervals[idx];
	}

	unsigned int samplesize = PAGESIZE / 2;
	if (is_freedive (parser->mode, parser->model)) {
		if (parser->model == F10A || parser->model == F10B ||
			parser->model == F11A || parser->model == F11B ||
			parser->model == MUNDIAL2 || parser->model == MUNDIAL3) {
			samplesize = 2;
		} else {
			samplesize = 4;
		}
	} else if (parser->model == OC1A || parser->model == OC1B ||
		parser->model == OC1C || parser->model == OCI ||
		parser->model == TX1 || parser->model == A300CS ||
		parser->model == VTX || parser->model == I450T ||
		parser->model == I750TC || parser->model == PROPLUSX ||
		parser->model == I770R || parser->model == I470TC ||
		parser->model == SAGE || parser->model == BEACON ||
		parser->model == GEOAIR || parser->model == I330R ||
		parser->model == I330R_C) {
		samplesize = PAGESIZE;
	} else if (parser->model == DSX) {
		samplesize = 32;
	}

	unsigned int have_temperature = 1, have_pressure = 1;
	if (is_freedive (parser->mode, parser->model)) {
		have_temperature = 0;
		have_pressure = 0;
	} else if (parser->model == VEO30 || parser->model == OCS ||
		parser->model == ELEMENT2 || parser->model == VEO20 ||
		parser->model == A300 || parser->model == ZEN ||
		parser->model == GEO || parser->model == GEO20 ||
		parser->model == MANTA || parser->model == I300 ||
		parser->model == I200 || parser->model == I100 ||
		parser->model == I300C || parser->model == TALIS ||
		parser->model == I200C || parser->model == I200CV2 ||
		parser->model == GEO40 || parser->model == VEO40 ||
		parser->model == I330R || parser->model == I330R_C ||
		parser->model == I100V2) {
		have_pressure = 0;
	}

	// Initial temperature.
	unsigned int temperature = 0;
	if (have_temperature) {
		temperature = data[parser->header + 7];
	}

	// Initial tank pressure.
	unsigned int tank = 1;
	unsigned int pressure = 0;
	if (have_pressure) {
		unsigned int idx = 2;
		if (parser->model == A300CS || parser->model == VTX ||
			parser->model == I750TC)
			idx = 16;
		pressure = array_uint16_le(data + parser->header + idx);
		if (pressure == 10000)
			have_pressure = 0;
	}

	// Initial gas mix.
	unsigned int gasmix_previous = 0xFFFFFFFF;

	unsigned int complete = 1;
	unsigned int previous = 0;
	unsigned int offset = parser->headersize;
	while (offset + samplesize <= size - parser->footersize) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if ((!is_freedive (parser->mode, parser->model) &&
			array_isequal (data + offset, samplesize, 0x00)) ||
			array_isequal (data + offset, samplesize, 0xFF)) {
			offset += samplesize;
			continue;
		}

		if (complete) {
			previous = offset;
			complete = 0;
		}

		// Get the sample type.
		unsigned int sampletype = data[offset + 0];
		if (is_freedive (parser->mode, parser->model))
			sampletype = 0;

		// The sample size is usually fixed, but some sample types have a
		// larger size. Check whether we have that many bytes available.
		unsigned int length = samplesize;
		if (sampletype == 0xBB) {
			length = PAGESIZE;
			if (offset + length > size - parser->footersize) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}
		}

		// Check for a tank switch sample.
		if (sampletype == 0xAA) {
			if (parser->model == DATAMASK || parser->model == COMPUMASK) {
				// Tank pressure (1 psi) and number
				tank = 1;
				pressure = (((data[offset + 7] << 8) + data[offset + 6]) & 0x0FFF);
			} else if (parser->model == A300CS || parser->model == VTX ||
				parser->model == I750TC || parser->model == SAGE ||
				parser->model == BEACON) {
				// Tank pressure (1 psi) and number (one based index)
				tank = data[offset + 1] & 0x03;
				pressure = ((data[offset + 7] << 8) + data[offset + 6]) & 0x0FFF;
			} else {
				// Tank pressure (2 psi) and number (one based index)
				tank = data[offset + 1] & 0x03;
				if (parser->model == ATOM2 || parser->model == EPICA || parser->model == EPICB)
					pressure = (((data[offset + 3] << 8) + data[offset + 4]) & 0x0FFF) * 2;
				else
					pressure = (((data[offset + 4] << 8) + data[offset + 5]) & 0x0FFF) * 2;
			}
		} else if (sampletype == 0xBB) {
			// The surface time is not always a nice multiple of the samplerate.
			// The number of inserted surface samples is therefore rounded down
			// to keep the timestamps aligned at multiples of the samplerate.
			unsigned int surftime = (60 * bcd2dec (data[offset + 1]) + bcd2dec (data[offset + 2])) * 1000;
			unsigned int nsamples = surftime / interval;

			for (unsigned int i = 0; i < nsamples; ++i) {
				// Time
				time += interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Vendor specific data
				if (i == 0) {
					oceanic_atom2_parser_vendor (parser,
						data + previous,
						(offset - previous) + length,
						samplesize, callback, userdata);
				}

				// Depth
				sample.depth = 0.0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
				complete = 1;
			}

			extratime += surftime;
		} else {
			// Time.
			if (parser->model == I450T || parser->model == I470TC) {
				unsigned int minute = bcd2dec(data[offset + 0]);
				unsigned int hour   = bcd2dec(data[offset + 1] & 0x0F);
				unsigned int second = bcd2dec(data[offset + 2]);
				unsigned int timestamp = ((hour * 3600) + (minute * 60 ) + second) * 1000 + extratime;
				if (timestamp < time) {
					ERROR (abstract->context, "Timestamp moved backwards.");
					return DC_STATUS_DATAFORMAT;
				} else 	if (timestamp == time) {
					WARNING (abstract->context, "Unexpected sample with the same timestamp ignored.");
					offset += length;
					continue;
				}
				time = timestamp;
			} else {
				time += interval;
			}
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Vendor specific data
			oceanic_atom2_parser_vendor (parser,
				data + previous,
				(offset - previous) + length,
				samplesize, callback, userdata);

			// Temperature (°F)
			if (have_temperature) {
				if (parser->model == GEO || parser->model == ATOM1 ||
					parser->model == ELEMENT2 || parser->model == MANTA ||
					parser->model == ZEN) {
					temperature = data[offset + 6];
				} else if (parser->model == TALIS) {
					temperature = data[offset + 7];
				} else if (parser->model == GEO20 || parser->model == VEO20 ||
					parser->model == VEO30 || parser->model == OC1A ||
					parser->model == OC1B || parser->model == OC1C ||
					parser->model == OCI || parser->model == A300 ||
					parser->model == I450T || parser->model == I300 ||
					parser->model == I200 || parser->model == I100 ||
					parser->model == I300C || parser->model == I200C ||
					parser->model == GEO40 || parser->model == VEO40 ||
					parser->model == I470TC || parser->model == I200CV2 ||
					parser->model == GEOAIR || parser->model == I100V2) {
					temperature = data[offset + 3];
				} else if (parser->model == OCS || parser->model == TX1) {
					temperature = data[offset + 1];
				} else if (parser->model == VT4 || parser->model == VT41 ||
					parser->model == ATOM3 || parser->model == ATOM31 ||
					parser->model == A300AI || parser->model == VISION ||
					parser->model == XPAIR) {
					temperature = ((data[offset + 7] & 0xF0) >> 4) | ((data[offset + 7] & 0x0C) << 2) | ((data[offset + 5] & 0x0C) << 4);
				} else if (parser->model == A300CS || parser->model == VTX ||
					parser->model == I750TC || parser->model == PROPLUSX ||
					parser->model == I770R|| parser->model == SAGE ||
					parser->model == BEACON) {
					temperature = data[offset + 11];
				} else if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
					temperature = array_uint16_le(data + offset + 10);
				} else {
					unsigned int sign;
					if (parser->model == DG03 || parser->model == PROPLUS3 ||
						parser->model == I550 || parser->model == I550C ||
						parser->model == PROPLUS4 || parser->model == WISDOM4)
						sign = (~data[offset + 5] & 0x04) >> 2;
					else if (parser->model == VOYAGER2G || parser->model == AMPHOS ||
						parser->model == AMPHOSAIR || parser->model == ZENAIR ||
						parser->model == AMPHOS2 || parser->model == AMPHOSAIR2)
						sign = (data[offset + 5] & 0x04) >> 2;
					else if (parser->model == ATOM2 || parser->model == PROPLUS21 ||
						parser->model == EPICA || parser->model == EPICB ||
						parser->model == ATMOSAI2 ||
						parser->model == WISDOM2 || parser->model == WISDOM3)
						sign = (data[offset + 0] & 0x80) >> 7;
					else
						sign = (~data[offset + 0] & 0x80) >> 7;
					if (sign)
						temperature -= (data[offset + 7] & 0x0C) >> 2;
					else
						temperature += (data[offset + 7] & 0x0C) >> 2;
				}
				if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
					sample.temperature = ((temperature / 10.0) - 32.0) * (5.0 / 9.0);
				} else {
					sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
				}
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}

			// Tank Pressure (psi)
			if (have_pressure) {
				if (parser->model == OC1A || parser->model == OC1B ||
					parser->model == OC1C || parser->model == OCI ||
					parser->model == I450T || parser->model == I470TC ||
					parser->model == GEOAIR)
					pressure = (data[offset + 10] + (data[offset + 11] << 8)) & 0x0FFF;
				else if (parser->model == VT4 || parser->model == VT41||
					parser->model == ATOM3 || parser->model == ATOM31 ||
					parser->model == ZENAIR ||parser->model == A300AI ||
					parser->model == DG03 || parser->model == PROPLUS3 ||
					parser->model == AMPHOSAIR || parser->model == I550 ||
					parser->model == VISION || parser->model == XPAIR ||
					parser->model == I550C || parser->model == PROPLUS4 ||
					parser->model == WISDOM4 || parser->model == AMPHOSAIR2)
					pressure = (((data[offset + 0] & 0x03) << 8) + data[offset + 1]) * 5;
				else if (parser->model == TX1 || parser->model == A300CS ||
					parser->model == VTX || parser->model == I750TC ||
					parser->model == PROPLUSX || parser->model == I770R ||
					parser->model == SAGE || parser->model == BEACON)
					pressure = array_uint16_le (data + offset + 4);
				else if (parser->model == DSX) {
					pressure = array_uint16_le (data + offset + 14);
					tank = (data[offset] & 0xF0) >> 4;
				} else {
					pressure -= data[offset + 1];
				}
				if (tank) {
					sample.pressure.tank = tank - 1;
					sample.pressure.value = pressure * PSI / BAR;
					if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
				}
			}

			// Depth (1/16 ft)
			unsigned int depth;
			if (is_freedive (parser->mode, parser->model))
				depth = array_uint16_le (data + offset);
			else if (parser->model == GEO20 || parser->model == VEO20 ||
				parser->model == VEO30 || parser->model == OC1A ||
				parser->model == OC1B || parser->model == OC1C ||
				parser->model == OCI || parser->model == A300 ||
				parser->model == I450T || parser->model == I300 ||
				parser->model == I200 || parser->model == I100 ||
				parser->model == I300C || parser->model == I200C ||
				parser->model == GEO40 || parser->model == VEO40 ||
				parser->model == I470TC || parser->model == I200CV2 ||
				parser->model == GEOAIR || parser->model == I100V2)
				depth = (data[offset + 4] + (data[offset + 5] << 8)) & 0x0FFF;
			else if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX)
				depth = array_uint16_le (data + offset + 2);
			else if (parser->model == ATOM1)
				depth = data[offset + 3] * 16;
			else
				depth = (data[offset + 2] + (data[offset + 3] << 8)) & 0x0FFF;
			if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
				sample.depth = depth / 10.0 * FEET;
			} else {
				sample.depth = depth / 16.0 * FEET;
			}
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Gas mix
			unsigned int have_gasmix = 0;
			unsigned int gasmix = 0;
			if (parser->model == TX1) {
				gasmix = data[offset] & 0x07;
				have_gasmix = 1;
			} else if (parser->model == DSX) {
				gasmix = (data[offset] & 0xF0) >> 4;
				have_gasmix = 1;
			}
			if (have_gasmix && gasmix != gasmix_previous && parser->ngasmixes > 0) {
				if (gasmix < 1 || gasmix > parser->ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix index (%u).", gasmix);
					return DC_STATUS_DATAFORMAT;
				}
				sample.gasmix = gasmix - 1;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix_previous = gasmix;
			}

			// NDL / Deco
			unsigned int have_deco = 0;
			unsigned int decostop = 0, decotime = 0;
			if (parser->model == A300CS || parser->model == VTX ||
				parser->model == I750TC || parser->model == SAGE ||
				parser->model == PROPLUSX || parser->model == I770R ||
				parser->model == BEACON) {
				decostop = (data[offset + 15] & 0x70) >> 4;
				decotime = array_uint16_le(data + offset + 6) & 0x03FF;
				have_deco = 1;
			} else if (parser->model == ZEN || parser->model == DG03) {
				decostop = (data[offset + 5] & 0xF0) >> 4;
				decotime = array_uint16_le(data + offset + 4) & 0x0FFF;
				have_deco = 1;
			} else if (parser->model == TX1) {
				decostop = data[offset + 10];
				decotime = array_uint16_le(data + offset + 6);
				have_deco = 1;
			} else if (parser->model == ATOM31 || parser->model == VISION ||
				parser->model == XPAIR || parser->model == I550 ||
				parser->model == I550C || parser->model == WISDOM4 ||
				parser->model == PROPLUS4 || parser->model == ATMOSAI2) {
				decostop = (data[offset + 5] & 0xF0) >> 4;
				decotime = array_uint16_le(data + offset + 4) & 0x03FF;
				have_deco = 1;
			} else if (parser->model == I200 || parser->model == I300 ||
				parser->model == OC1A || parser->model == OC1B ||
				parser->model == OC1C || parser->model == OCI ||
				parser->model == I100 || parser->model == I300C ||
				parser->model == I450T || parser->model == I200C ||
				parser->model == GEO40 || parser->model == VEO40 ||
				parser->model == I470TC || parser->model == I200CV2 ||
				parser->model == GEOAIR || parser->model == I100V2) {
				decostop = (data[offset + 7] & 0xF0) >> 4;
				decotime = array_uint16_le(data + offset + 6) & 0x0FFF;
				have_deco = 1;
			} else if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
				decostop = data[offset + 8];
				if (decostop) {
					// Deco time
					decotime = array_uint16_le(data + offset + 6);
				} else {
					// NDL
					decotime = array_uint16_le(data + offset + 4);
				}
				have_deco = 1;
			}
			if (have_deco) {
				if (decostop) {
					sample.deco.type = DC_DECO_DECOSTOP;
					if (parser->model == I330R || parser->model == I330R_C || parser->model == DSX) {
						sample.deco.depth = decostop * FEET;
					} else {
						sample.deco.depth = decostop * 10 * FEET;
					}
				} else {
					sample.deco.type = DC_DECO_NDL;
					sample.deco.depth = 0.0;
				}
				sample.deco.time = decotime * 60;
				sample.deco.tts = 0;
				if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
			}

			unsigned int have_rbt = 0;
			unsigned int rbt = 0;
			if (parser->model == ATOM31) {
				rbt = array_uint16_le(data + offset + 6) & 0x01FF;
				have_rbt = 1;
			} else if (parser->model == I450T || parser->model == OC1A ||
				parser->model == OC1B || parser->model == OC1C ||
				parser->model == OCI || parser->model == PROPLUSX ||
				parser->model == I770R || parser->model == I470TC ||
				parser->model == GEOAIR) {
				rbt = array_uint16_le(data + offset + 8) & 0x01FF;
				have_rbt = 1;
			} else if (parser->model == VISION || parser->model == XPAIR ||
				parser->model == I550 || parser->model == I550C ||
				parser->model == WISDOM4 || parser->model == PROPLUS4 ||
				parser->model == ATMOSAI2) {
				rbt = array_uint16_le(data + offset + 6) & 0x03FF;
				have_rbt = 1;
			}
			if (have_rbt) {
				sample.rbt = rbt;
				if (callback) callback (DC_SAMPLE_RBT, &sample, userdata);
			}

			// PPO2
			if (parser->model == I330R || parser->model == I330R_C) {
				sample.ppo2.sensor = DC_SENSOR_NONE;
				sample.ppo2.value = data[offset + 9] / 100.0;
				if (callback) callback (DC_SAMPLE_PPO2, &sample, userdata);
			}

			// Bookmarks
			unsigned int have_bookmark = 0;
			if (parser->model == OC1A || parser->model == OC1B ||
				parser->model == OC1C || parser->model == OCI ||
				parser->model == GEOAIR) {
				have_bookmark = data[offset + 12] & 0x80;
			}
			if (have_bookmark) {
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			}

			complete = 1;
		}

		offset += length;
	}

	return DC_STATUS_SUCCESS;
}
