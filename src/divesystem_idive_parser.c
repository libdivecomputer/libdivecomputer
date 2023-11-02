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

#include "divesystem_idive.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &divesystem_idive_parser_vtable)

#define ISIX3M(model) ((model) >= 0x21)
#define ISIX3M2(model) ((model) >= 0x60 && (model) < 0x1000)

#define SZ_HEADER_IDIVE 0x32
#define SZ_SAMPLE_IDIVE 0x2A
#define SZ_HEADER_IX3M  0x36
#define SZ_SAMPLE_IX3M  0x36
#define SZ_SAMPLE_IX3M_APOS4 0x40

#define NGASMIXES 8
#define NTANKS    10

#define EPOCH 1199145600 /* 2008-01-01 00:00:00 */

#define OC       0
#define SCR      1
#define CCR      2
#define GAUGE    3
#define FREEDIVE 4
#define INVALID  0xFFFFFFFF

#define BUHLMANN 0
#define VPM      1
#define DUAL     2

#define IX3M2_BUHLMANN 0
#define IX3M2_ZHL16B   1
#define IX3M2_ZHL16C   2
#define IX3M2_VPM      3

#define REC_SAMPLE 0
#define REC_INFO   1

typedef struct divesystem_idive_parser_t divesystem_idive_parser_t;

typedef struct divesystem_idive_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} divesystem_idive_gasmix_t;

typedef struct divesystem_idive_tank_t {
	unsigned int id;
	unsigned int beginpressure;
	unsigned int endpressure;
} divesystem_idive_tank_t;

struct divesystem_idive_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int headersize;
	// Cached fields.
	unsigned int cached;
	unsigned int divemode;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int ngasmixes;
	unsigned int ntanks;
	divesystem_idive_gasmix_t gasmix[NGASMIXES];
	divesystem_idive_tank_t tank[NTANKS];
	unsigned int algorithm;
	unsigned int gf_low;
	unsigned int gf_high;
	unsigned int have_location;
	int latitude;
	int longitude;
	int altitude;
};

static dc_status_t divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t divesystem_idive_parser_vtable = {
	sizeof(divesystem_idive_parser_t),
	DC_FAMILY_DIVESYSTEM_IDIVE,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	divesystem_idive_parser_get_datetime, /* datetime */
	divesystem_idive_parser_get_field, /* fields */
	divesystem_idive_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
divesystem_idive_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	divesystem_idive_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (divesystem_idive_parser_t *) dc_parser_allocate (context, &divesystem_idive_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	if (ISIX3M(model)) {
		parser->headersize = SZ_HEADER_IX3M;
	} else {
		parser->headersize = SZ_HEADER_IDIVE;
	}
	parser->cached = 0;
	parser->divemode = INVALID;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->ngasmixes = 0;
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].id = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
	}
	parser->algorithm = INVALID;
	parser->gf_low = INVALID;
	parser->gf_high = INVALID;
	parser->have_location = 0;
	parser->latitude = 0;
	parser->longitude = 0;
	parser->altitude = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;

	static const signed char tz_array[] = {
		-12,  0,    /* UTC-12    */
		-11,  0,    /* UTC-11    */
		-10,  0,    /* UTC-10    */
		 -9, 30,    /* UTC-9:30  */
		 -9,  0,    /* UTC-9     */
		 -8,  0,    /* UTC-8     */
		 -7,  0,    /* UTC-7     */
		 -6,  0,    /* UTC-6     */
		 -5,  0,    /* UTC-5     */
		 -4, 30,    /* UTC-4:30  */
		 -4,  0,    /* UTC-4     */
		 -3, 30,    /* UTC-3:30  */
		 -3,  0,    /* UTC-3     */
		 -2,  0,    /* UTC-2     */
		 -1,  0,    /* UTC-1     */
		  0,  0,    /* UTC       */
		  1,  0,    /* UTC+1     */
		  2,  0,    /* UTC+2     */
		  3,  0,    /* UTC+3     */
		  3, 30,    /* UTC+3:30  */
		  4,  0,    /* UTC+4     */
		  4, 30,    /* UTC+4:30  */
		  5,  0,    /* UTC+5     */
		  5, 30,    /* UTC+5:30  */
		  5, 45,    /* UTC+5:45  */
		  6,  0,    /* UTC+6     */
		  6, 30,    /* UTC+6:30  */
		  7,  0,    /* UTC+7     */
		  8,  0,    /* UTC+8     */
		  8, 45,    /* UTC+8:45  */
		  9,  0,    /* UTC+9     */
		  9, 30,    /* UTC+9:30  */
		  9, 45,    /* UTC+9:45  */
		 10,  0,    /* UTC+10    */
		 10, 30,    /* UTC+10:30 */
		 11,  0,    /* UTC+11    */
		 11, 30,    /* UTC+11:30 */
		 12,  0,    /* UTC+12    */
		 12, 45,    /* UTC+12:45 */
		 13,  0,    /* UTC+13    */
		 13, 45,    /* UTC+13:45 */
		 14,  0     /* UTC+14    */
	};

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	dc_ticks_t ticks = array_uint32_le(abstract->data + 7) + EPOCH;

	// Detect the APOS4 firmware.
	unsigned int firmware = 0;
	unsigned int apos4 = 0;
	if (ISIX3M(parser->model)) {
		firmware = array_uint32_le(abstract->data + 0x2A);
		apos4 = (firmware / 10000000) >= 4;
	} else {
		firmware = array_uint32_le(abstract->data + 0x2E);
		apos4 = 0;
	}

	if (apos4) {
		// For devices with timezone support, the UTC offset of the
		// device is used. The UTC offset is stored as an index in the
		// timezone table.
		unsigned int tz_idx = abstract->data[48];
		if ((tz_idx % 2) != 0 || tz_idx >= C_ARRAY_SIZE(tz_array)) {
			ERROR (abstract->context, "Invalid timezone index (%u).", tz_idx);
			return DC_STATUS_DATAFORMAT;
		}

		int timezone = tz_array[tz_idx] * 3600;
		if (timezone < 0) {
			timezone -= tz_array[tz_idx + 1] * 60;
		} else {
			timezone += tz_array[tz_idx + 1] * 60;
		}

		ticks += timezone;

		if (!dc_datetime_gmtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;

		datetime->timezone = timezone;
	} else {
		// For devices without timezone support, the current timezone of
		// the host system is used.
		if (!dc_datetime_localtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		dc_status_t rc = divesystem_idive_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;
	dc_location_t *location = (dc_location_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->beginpressure = parser->tank[flags].beginpressure;
			tank->endpressure   = parser->tank[flags].endpressure;
			tank->gasmix = DC_GASMIX_UNKNOWN;
			tank->usage = DC_USAGE_NONE;
			break;
		case DC_FIELD_ATMOSPHERIC:
			if (ISIX3M(parser->model)) {
				*((double *) value) = array_uint16_le (data + 11) / 10000.0;
			} else {
				*((double *) value) = array_uint16_le (data + 11) / 1000.0;
			}
			break;
		case DC_FIELD_SALINITY:
			water->type = data[34] == 0 ? DC_WATER_SALT : DC_WATER_FRESH;
			water->density = 0.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (parser->divemode == INVALID)
				return DC_STATUS_UNSUPPORTED;
			switch (parser->divemode) {
			case OC:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case SCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
				break;
			case CCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case FREEDIVE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				ERROR (abstract->context, "Unknown dive mode %02x.", parser->divemode);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_DECOMODEL:
			if (parser->algorithm == INVALID)
				return DC_STATUS_UNSUPPORTED;
			if (ISIX3M2(parser->model)) {
				switch (parser->algorithm) {
				case IX3M2_BUHLMANN:
				case IX3M2_ZHL16B:
				case IX3M2_ZHL16C:
					decomodel->type = DC_DECOMODEL_BUHLMANN;
					decomodel->conservatism = 0;
					decomodel->params.gf.low = parser->gf_low;
					decomodel->params.gf.high = parser->gf_high;
					break;
				case IX3M2_VPM:
					decomodel->type = DC_DECOMODEL_VPM;
					decomodel->conservatism = 0;
					break;
				default:
					ERROR (abstract->context, "Unknown deco algorithm %02x.", parser->algorithm);
					return DC_STATUS_DATAFORMAT;
				}
			} else {
				switch (parser->algorithm) {
				case BUHLMANN:
				case DUAL:
					decomodel->type = DC_DECOMODEL_BUHLMANN;
					decomodel->conservatism = 0;
					decomodel->params.gf.low = parser->gf_low;
					decomodel->params.gf.high = parser->gf_high;
					break;
				case VPM:
					decomodel->type = DC_DECOMODEL_VPM;
					decomodel->conservatism = 0;
					break;
				default:
					ERROR (abstract->context, "Unknown deco algorithm %02x.", parser->algorithm);
					return DC_STATUS_DATAFORMAT;
				}
			}
			break;
		case DC_FIELD_LOCATION:
			if (!parser->have_location)
				return DC_STATUS_UNSUPPORTED;
			location->latitude  = parser->latitude  / 10000000.0;
			location->longitude = parser->longitude / 10000000.0;
			location->altitude  = parser->altitude  / 1000.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int maxdepth = 0;
	unsigned int ngasmixes = 0;
	unsigned int ntanks = 0;
	divesystem_idive_gasmix_t gasmix[NGASMIXES] = {0};
	divesystem_idive_tank_t tank[NTANKS] = {0};
	unsigned int o2_previous = INVALID;
	unsigned int he_previous = INVALID;
	unsigned int mode_previous = INVALID;
	unsigned int divemode = INVALID;
	unsigned int tank_previous = INVALID;
	unsigned int tank_idx = INVALID;
	unsigned int algorithm = INVALID;
	unsigned int algorithm_previous = INVALID;
	unsigned int gf_low = INVALID;
	unsigned int gf_high = INVALID;
	unsigned int have_bearing = 0;

	unsigned int firmware = 0;
	unsigned int apos4 = 0;
	unsigned int nsamples = array_uint16_le (data + 1);
	unsigned int samplesize = SZ_SAMPLE_IDIVE;
	if (ISIX3M(parser->model)) {
		// Detect the APOS4 firmware.
		firmware = array_uint32_le(data + 0x2A);
		apos4 = (firmware / 10000000) >= 4;
		if (apos4) {
			// Dive downloaded and recorded with the APOS4 firmware.
			samplesize = SZ_SAMPLE_IX3M_APOS4;
		} else if (size == parser->headersize + nsamples * SZ_SAMPLE_IX3M_APOS4) {
			// Dive downloaded with the APOS4 firmware, but recorded
			// with an older firmware.
			samplesize = SZ_SAMPLE_IX3M_APOS4;
		} else {
			// Dive downloaded and recorded with an older firmware.
			samplesize = SZ_SAMPLE_IX3M;
		}
	} else {
		firmware = array_uint32_le(data + 0x2E);
	}

	unsigned int have_location = 0;
	int altitude = 0, longitude = 0, latitude = 0;

	unsigned int offset = parser->headersize;
	while (offset + samplesize <= size) {
		dc_sample_value_t sample = {0};

		// Get the record type.
		unsigned int type = ISIX3M(parser->model) ?
			array_uint16_le (data + offset + 52) :
			REC_SAMPLE;
		if (type != REC_SAMPLE) {
			if (type == REC_INFO) {
				if (!have_location) {
					altitude  = (signed int) array_uint32_le (data + offset + 40);
					longitude = (signed int) array_uint32_le (data + offset + 44);
					latitude  = (signed int) array_uint32_le (data + offset + 48);
					have_location = 1;
				} else {
					WARNING (abstract->context, "Multiple GPS locations present.");
				}
			}

			// Skip non-sample records.
			offset += samplesize;
			continue;
		}

		// Time (seconds).
		unsigned int timestamp = array_uint32_le (data + offset + 2);
		if (timestamp <= time && time != 0) {
			ERROR (abstract->context, "Timestamp moved backwards (%u %u).", timestamp, time);
			return DC_STATUS_DATAFORMAT;
		}
		time = timestamp;
		sample.time = timestamp * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_le (data + offset + 6);
		if (maxdepth < depth)
			maxdepth = depth;
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Temperature (Celsius).
		signed int temperature = (signed short) array_uint16_le (data + offset + 8);
		sample.temperature = temperature / 10.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

		// Dive mode
		unsigned int mode = data[offset + 18];
		if (mode != mode_previous) {
			if (mode_previous != INVALID) {
				WARNING (abstract->context, "Dive mode changed from %02x to %02x.", mode_previous, mode);
			}
			mode_previous = mode;
		}
		if (divemode == INVALID) {
			divemode = mode;
		}

		// Deco model
		unsigned int s_algorithm = data[offset + 14];
		unsigned int s_gf_high = data[offset + 15];
		unsigned int s_gf_low  = data[offset + 16];
		if (s_algorithm != algorithm_previous) {
			if (algorithm_previous != INVALID) {
				WARNING (abstract->context, "Deco algorithm changed from %02x to %02x.", algorithm_previous, s_algorithm);
			}
			algorithm_previous = s_algorithm;
		}
		if (algorithm == INVALID) {
			algorithm = s_algorithm;
			gf_low = s_gf_low;
			gf_high = s_gf_high;
		}

		// Setpoint
		if (mode == SCR || mode == CCR) {
			unsigned int setpoint = array_uint16_le (data + offset + 19);
			sample.setpoint = setpoint / 1000.0;
			if (callback) callback (DC_SAMPLE_SETPOINT, &sample, userdata);
		}

		// Gaschange.
		unsigned int o2 = data[offset + 10];
		unsigned int he = data[offset + 11];
		if (o2 != o2_previous || he != he_previous) {
			// Find the gasmix in the list.
			unsigned int i = 0;
			while (i < ngasmixes) {
				if (o2 == gasmix[i].oxygen && he == gasmix[i].helium)
					break;
				i++;
			}

			// Add it to list if not found.
			if (i >= ngasmixes) {
				if (i >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				gasmix[i].oxygen = o2;
				gasmix[i].helium = he;
				ngasmixes = i + 1;
			}

			sample.gasmix = i;
			if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int decostop = 0, decotime = 0, tts = 0;
		if (apos4) {
			decostop = array_uint16_le (data + offset + 21);
			decotime = array_uint16_le (data + offset + 23);
			tts      = array_uint16_le (data + offset + 25);
		} else {
			decostop = array_uint16_le (data + offset + 21);
			tts      = array_uint16_le (data + offset + 23);
		}
		if (decostop) {
			sample.deco.type = DC_DECO_DECOSTOP;
			sample.deco.depth = decostop / 10.0;
			sample.deco.time = decotime;
			sample.deco.tts = tts;
		} else {
			sample.deco.type = DC_DECO_NDL;
			sample.deco.depth = 0.0;
			sample.deco.time = tts;
			sample.deco.tts = 0;
		}
		if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);

		// CNS
		unsigned int cns = array_uint16_le (data + offset + 29);
		sample.cns = cns / 100.0;
		if (callback) callback (DC_SAMPLE_CNS, &sample, userdata);

		// Tank Pressure
		if (samplesize == SZ_SAMPLE_IX3M_APOS4) {
			unsigned int id = data[offset + 47] & 0x0F;
			unsigned int flags = data[offset + 47] & 0xF0;
			unsigned int pressure = data[offset + 49];

			if (flags & 0x20) {
				// 300 bar transmitter.
				pressure *= 2;
			}

			if (flags & 0x80) {
				// No active transmitter available
			} else if (flags & 0x40) {
				// Transmitter connection lost
				sample.event.type = SAMPLE_EVENT_TRANSMITTER;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			} else {
				// Get the index of the tank.
				if (id != tank_previous) {
					unsigned int i = 0;
					while (i < ntanks) {
						if (id == tank[i].id)
							break;
						i++;
					}

					tank_previous = id;
					tank_idx = i;
				}

				// Add a new tank if necessary.
				if (tank_idx >= ntanks && pressure != 0) {
					if (tank_idx >= NTANKS) {
						ERROR (abstract->context, "Maximum number of tanks reached.");
						return DC_STATUS_DATAFORMAT;
					}
					tank[tank_idx].id = id;
					tank[tank_idx].beginpressure = pressure;
					tank[tank_idx].endpressure = pressure;
					ntanks = tank_idx + 1;
				}

				if (tank_idx < ntanks) {
					sample.pressure.tank = tank_idx;
					sample.pressure.value = pressure;
					if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
					tank[tank_idx].endpressure = pressure;
				}
			}

			// Compass bearing
			unsigned int bearing = array_uint16_le (data + offset + 50);
			if (bearing != 0) {
				have_bearing = 1; // Stop ignoring zero values.
			}
			if (have_bearing && bearing != 0xFFFF) {
				sample.bearing = bearing;
				if (callback) callback (DC_SAMPLE_BEARING, &sample, userdata);
			}
		}

		offset += samplesize;
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ntanks; ++i) {
		parser->tank[i] = tank[i];
	}
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->ntanks = ntanks;
	parser->maxdepth = maxdepth;
	parser->divetime = time;
	parser->divemode = divemode;
	parser->algorithm = algorithm;
	parser->gf_low = gf_low;
	parser->gf_high = gf_high;
	parser->have_location = have_location;
	parser->latitude = latitude;
	parser->longitude = longitude;
	parser->altitude = altitude;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
