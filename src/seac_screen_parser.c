/*
 * libdivecomputer
 *
 * Copyright (C) 2022 Jef Driesen
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

#include "seac_screen.h"
#include "context-private.h"
#include "parser-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &seac_screen_parser_vtable)

#define SZ_HEADER 128
#define SZ_SAMPLE  64

#define NGASMIXES 2

#define INVALID 0xFFFFFFFF

typedef struct seac_screen_parser_t seac_screen_parser_t;

struct seac_screen_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int gf_low;
	unsigned int gf_high;
};

static dc_status_t seac_screen_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t seac_screen_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t seac_screen_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t seac_screen_parser_vtable = {
	sizeof(seac_screen_parser_t),
	DC_FAMILY_SEAC_SCREEN,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	seac_screen_parser_get_datetime, /* datetime */
	seac_screen_parser_get_field, /* fields */
	seac_screen_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
seac_screen_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	seac_screen_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (seac_screen_parser_t *) dc_parser_allocate (context, &seac_screen_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
	}
	parser->gf_low = 0;
	parser->gf_high = 0;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;

	static const signed int tz_array[] = {
		-12 * 60,         /* UTC-12    */
		-11 * 60,         /* UTC-11    */
		-10 * 60,         /* UTC-10    */
		 -9 * 60 - 30,    /* UTC-9:30  */
		 -9 * 60,         /* UTC-9     */
		 -8 * 60,         /* UTC-8     */
		 -7 * 60,         /* UTC-7     */
		 -6 * 60,         /* UTC-6     */
		 -5 * 60,         /* UTC-5     */
		 -4 * 60 - 30,    /* UTC-4:30  */
		 -4 * 60,         /* UTC-4     */
		 -3 * 60 - 30,    /* UTC-3:30  */
		 -3 * 60,         /* UTC-3     */
		 -2 * 60,         /* UTC-2     */
		 -1 * 60,         /* UTC-1     */
		  0 * 60,         /* UTC       */
		  1 * 60,         /* UTC+1     */
		  2 * 60,         /* UTC+2     */
		  3 * 60,         /* UTC+3     */
		  3 * 60 + 30,    /* UTC+3:30  */
		  4 * 60,         /* UTC+4     */
		  4 * 60 + 30,    /* UTC+4:30  */
		  5 * 60,         /* UTC+5     */
		  5 * 60 + 30,    /* UTC+5:30  */
		  5 * 60 + 45,    /* UTC+5:45  */
		  6 * 60,         /* UTC+6     */
		  6 * 60 + 30,    /* UTC+6:30  */
		  7 * 60,         /* UTC+7     */
		  8 * 60,         /* UTC+8     */
		  8 * 60 + 45,    /* UTC+8:45  */
		  9 * 60,         /* UTC+9     */
		  9 * 60 + 30,    /* UTC+9:30  */
		  9 * 60 + 45,    /* UTC+9:45  */
		 10 * 60,         /* UTC+10    */
		 10 * 60 + 30,    /* UTC+10:30 */
		 11 * 60,         /* UTC+11    */
		 11 * 60 + 30,    /* UTC+11:30 */
		 12 * 60,         /* UTC+12    */
		 12 * 60 + 45,    /* UTC+12:45 */
		 13 * 60,         /* UTC+13    */
		 13 * 60 + 45,    /* UTC+13:45 */
		 14 * 60,         /* UTC+14    */
	};

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	// The date/time is stored as UTC time with a timezone offset. To convert to
	// local time, the UTC time is first converted to unix time (seconds since
	// the epoch), then adjusted for the timezone offset, and finally converted
	// back into the broken-down time format.

	dc_datetime_t utc = {0};
	utc.year   = data[0x10] + 2000;
	utc.month  = data[0x0F];
	utc.day    = data[0x0E];
	utc.hour   = data[0x0B];
	utc.minute = data[0x0C];
	utc.second = data[0x0D];
	utc.timezone = 0;

	unsigned int tz_idx = data[0x0A];
	if (tz_idx >= C_ARRAY_SIZE(tz_array)) {
		ERROR (abstract->context, "Invalid timezone index (%u).", tz_idx);
		return DC_STATUS_DATAFORMAT;
	}
	int timezone = tz_array[tz_idx] * 60;

	dc_ticks_t ticks = dc_datetime_mktime (&utc);
	if (ticks == -1)
		return DC_STATUS_DATAFORMAT;

	ticks += timezone;

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = timezone;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	seac_screen_parser_t *parser = (seac_screen_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		dc_status_t rc = seac_screen_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_AVGDEPTH:
			*((double *) value) = array_uint16_le (data + 0x72) / 100.0;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 0x62) / 100.0;
			break;
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint32_le (data + 0x64);
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = array_uint16_le (data + 0x6A) / 100.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = array_uint16_le (data + 0x68) / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *)value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			switch (data[0x26]) {
			case 1:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case 2:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case 3:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				ERROR (abstract->context, "Unknown dive mode %i", data[0x26]);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_DECOMODEL:
			decomodel->type = DC_DECOMODEL_BUHLMANN;
			decomodel->conservatism = 0;
			decomodel->params.gf.low = parser->gf_low;
			decomodel->params.gf.high = parser->gf_high;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	seac_screen_parser_t *parser = (seac_screen_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (checksum_crc16_ccitt (data, SZ_HEADER / 2, 0xFFFF, 0x0000) != 0 ||
		checksum_crc16_ccitt (data + SZ_HEADER / 2, SZ_HEADER / 2, 0xFFFF, 0x0000) != 0) {
		ERROR (abstract->context, "Unexpected header checksum.");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int dive_id = array_uint32_le (data + 0x00);

	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	unsigned int o2_previous = INVALID;

	unsigned int gf_low = 0;
	unsigned int gf_high = 0;

	unsigned int time = 0;
	unsigned int offset = SZ_HEADER;
	while (offset + SZ_SAMPLE <= size) {
		dc_sample_value_t sample = {0};

		if (checksum_crc16_ccitt (data + offset, SZ_SAMPLE, 0xFFFF, 0x0000) != 0) {
			ERROR (abstract->context, "Unexpected sample checksum.");
			return DC_STATUS_DATAFORMAT;
		}

		unsigned int id          = array_uint32_le (data + offset + 0x00);
		unsigned int timestamp   = array_uint32_le (data + offset + 0x04);
		unsigned int depth       = array_uint16_le (data + offset + 0x08);
		unsigned int temperature = array_uint16_le (data + offset + 0x0A);
		unsigned int o2          = data[offset + 0x0C];
		unsigned int decodepth   = array_uint16_le (data + offset + 0x0E);
		unsigned int decotime    = array_uint16_le (data + offset + 0x10);
		unsigned int ndl_tts     = array_uint16_le (data + offset + 0x12);
		unsigned int cns         = array_uint16_le (data + offset + 0x16);
		unsigned int gf_hi       = data[offset + 0x3B];
		unsigned int gf_lo       = data[offset + 0x3C];

		if (id != dive_id) {
			ERROR (abstract->context, "Unexpected sample id (%u %u).", dive_id, id);
			return DC_STATUS_DATAFORMAT;
		}

		// Time (seconds).
		if (timestamp < time) {
			ERROR (abstract->context, "Timestamp moved backwards (%u %u).", timestamp, time);
			return DC_STATUS_DATAFORMAT;
		}
		time = timestamp;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth (1/100 m).
		sample.depth = depth / 100.0;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Temperature (1/100 °C).
		sample.temperature = temperature / 100.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

		// Gas mix
		if (o2 != o2_previous) {
			// Find the gasmix in the list.
			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (o2 == oxygen[idx])
					break;
				idx++;
			}

			// Add it to list if not found.
			if (idx >= ngasmixes) {
				if (idx >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				oxygen[idx] = o2;
				ngasmixes = idx + 1;
			}

			sample.gasmix = idx;
			if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
			o2_previous = o2;
		}

		// NDL / Deco
		if (decodepth) {
			sample.deco.type = DC_DECO_DECOSTOP;
			sample.deco.time = decotime;
			sample.deco.depth = decodepth;
		} else {
			sample.deco.type = DC_DECO_NDL;
			sample.deco.time = ndl_tts;
			sample.deco.depth = 0;
		}
		sample.deco.tts = 0;
		if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);

		// CNS
		sample.cns = cns / 100.0;
		if (callback) callback (DC_SAMPLE_CNS, &sample, userdata);

		// Deco model
		if (gf_low == 0 && gf_high == 0) {
			gf_low = gf_lo;
			gf_high = gf_hi;
		}

		offset += SZ_SAMPLE;
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->gf_low = gf_low;
	parser->gf_high = gf_high;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
