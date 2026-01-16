/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Jef Driesen
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

#include "halcyon_symbios.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"
#include "array.h"

#define ID_HEADER          0x01
#define ID_GAS_SWITCH      0x02
#define ID_DEPTH           0x03
#define ID_TEMPERATURE     0x04
#define ID_OC_CC_SWITCH    0x05
#define ID_GAS_TRANSMITTER 0x06
#define ID_COMPARTMENTS    0x07
#define ID_GPS             0x08
#define ID_PO2_BOARD       0x09
#define ID_DECO            0x0A
#define ID_GF              0x0B
#define ID_FOOTER          0x0C
#define ID_PO2_REBREATHER  0x0D
#define ID_COMPASS         0x0E
#define ID_LOG_VERSION     0x0F
#define ID_TRIM            0x10
#define ID_GAS_CONFIG      0x11
#define ID_TANK_TRANSMITTER 0x12
#define ID_GF_INFO          0x13
#define ID_SGC             0x14
#define ID_GF_DATA         0x15

#define ISCONFIG(type) ( \
	(type) == ID_LOG_VERSION || \
	(type) == ID_HEADER || \
	(type) == ID_FOOTER)

#define LOGVERSION(major,minor) ( \
		(((major) & 0xFF) << 8) | \
		((minor) & 0xFF))

#define UNDEFINED 0xFFFFFFFF

#define EPOCH 1609459200 /* 2021-01-01 00:00:00 */

#define OC        0
#define CCR       1
#define CCR_FSP   2
#define SIDEMOUNT 3
#define GAUGE     4

#define NGASMIXES 10
#define NTANKS    10

#define TRANSMITTER_ID (1u << 16)

typedef struct halcyon_symbios_gasmix_t {
	unsigned int id;
	unsigned int oxygen;
	unsigned int helium;
} halcyon_symbios_gasmix_t;

typedef struct halcyon_symbios_tank_t {
	unsigned int id;
	unsigned int beginpressure;
	unsigned int endpressure;
	unsigned int gasmix;
	dc_usage_t usage;
} halcyon_symbios_tank_t;

typedef struct halcyon_symbios_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int logversion;
	unsigned int datetime;
	int timezone;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int divemode;
	unsigned int atmospheric;
	unsigned int ngasmixes;
	unsigned int ntanks;
	halcyon_symbios_gasmix_t gasmix[NGASMIXES];
	halcyon_symbios_tank_t tank[NTANKS];
	unsigned int gf_lo;
	unsigned int gf_hi;
	unsigned int have_location;
	int latitude, longitude;
} halcyon_symbios_parser_t;

static dc_status_t halcyon_symbios_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t halcyon_symbios_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t halcyon_symbios_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t halcyon_symbios_parser_vtable = {
	sizeof(halcyon_symbios_parser_t),
	DC_FAMILY_HALCYON_SYMBIOS,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	halcyon_symbios_parser_get_datetime, /* datetime */
	halcyon_symbios_parser_get_field, /* fields */
	halcyon_symbios_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
halcyon_symbios_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	halcyon_symbios_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (halcyon_symbios_parser_t *) dc_parser_allocate (context, &halcyon_symbios_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->logversion = 0;
	parser->datetime = UNDEFINED;
	parser->timezone = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->divemode = UNDEFINED;
	parser->atmospheric = UNDEFINED;
	parser->gf_lo = UNDEFINED;
	parser->gf_hi = UNDEFINED;
	parser->have_location = 0;
	parser->latitude = 0;
	parser->longitude = 0;
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
		parser->tank[i].gasmix = DC_GASMIX_UNKNOWN;
		parser->tank[i].usage = DC_USAGE_NONE;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
halcyon_symbios_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	halcyon_symbios_parser_t *parser = (halcyon_symbios_parser_t *) abstract;

	// Cache the profile data.
	if (!parser->cached) {
		status = halcyon_symbios_parser_samples_foreach (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	if (parser->datetime == UNDEFINED)
		return DC_STATUS_UNSUPPORTED;

	dc_ticks_t ticks = (dc_ticks_t) parser->datetime + EPOCH;

	if (parser->logversion >= LOGVERSION(1,9)) {
		// For firmware versions with timezone support, the UTC offset of the
		// device is used.
		int timezone = parser->timezone * 3600;

		ticks += timezone;

		if (!dc_datetime_gmtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;

		datetime->timezone = timezone;
	} else {
		// For firmware versions without timezone support, the current timezone
		// of the host system is used.
		if (!dc_datetime_localtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
halcyon_symbios_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	halcyon_symbios_parser_t *parser = (halcyon_symbios_parser_t *) abstract;

	// Cache the profile data.
	if (!parser->cached) {
		status = halcyon_symbios_parser_samples_foreach (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;
	dc_location_t *location = (dc_location_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth / 100.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			if (parser->atmospheric == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = parser->atmospheric / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->divemode) {
			case OC:
			case SIDEMOUNT:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case CCR:
			case CCR_FSP:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
				break;
			case GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case UNDEFINED:
				return DC_STATUS_UNSUPPORTED;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			gasmix->usage = DC_USAGE_NONE;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->beginpressure = parser->tank[flags].beginpressure / 10.0;
			tank->endpressure   = parser->tank[flags].endpressure   / 10.0;
			tank->gasmix        = parser->tank[flags].gasmix;
			tank->usage         = parser->tank[flags].usage;
			break;
		case DC_FIELD_DECOMODEL:
			if (parser->gf_lo == UNDEFINED || parser->gf_hi == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			decomodel->type = DC_DECOMODEL_BUHLMANN;
			decomodel->conservatism = 0;
			decomodel->params.gf.low = parser->gf_lo;
			decomodel->params.gf.high = parser->gf_hi;
			break;
		case DC_FIELD_LOCATION:
			if (!parser->have_location)
				return DC_STATUS_UNSUPPORTED;
			location->latitude  = parser->latitude  / 1000000.0;
			location->longitude = parser->longitude / 1000000.0;
			location->altitude  = 0.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
halcyon_symbios_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	halcyon_symbios_parser_t *parser = (halcyon_symbios_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	static const unsigned int lengths[] = {
		4,  /* ID_LOG_VERSION */
		64, /* ID_HEADER */
		4,  /* ID_GAS_SWITCH */
		4,  /* ID_DEPTH */
		4,  /* ID_TEMPERATURE */
		4,  /* ID_OC_CC_SWITCH */
		12, /* ID_GAS_TRANSMITTER */
		68, /* ID_COMPARTMENTS */
		12, /* ID_GPS */
		8,  /* ID_PO2_BOARD */
		16, /* ID_DECO */
		4,  /* ID_GF */
		16, /* ID_FOOTER */
		12, /* ID_PO2_REBREATHER */
		4,  /* ID_COMPASS */
		4,  /* ID_LOG_VERSION */
		4,  /* ID_TRIM */
		8,  /* ID_GAS_CONFIG */
		8,  /* ID_TANK_TRANSMITTER */
		6,  /* ID_GF_INFO */
		4,  /* ID_SGC */
		8,  /* ID_GF_DATA */
	};

	unsigned int logversion = 0;
	unsigned int time_start = UNDEFINED, time_end = UNDEFINED;
	int timezone = 0;
	unsigned int maxdepth = 0;
	unsigned int divemode = UNDEFINED;
	unsigned int atmospheric = UNDEFINED;
	unsigned int gf_lo = UNDEFINED;
	unsigned int gf_hi = UNDEFINED;
	unsigned int have_location = 0;
	int latitude = 0, longitude = 0;
	unsigned int ngasmixes = 0;
	unsigned int ntanks = 0;
	halcyon_symbios_gasmix_t gasmix[NGASMIXES] = {0};
	halcyon_symbios_tank_t tank[NTANKS] = {0};
	unsigned int gasmix_id_previous = UNDEFINED;
	unsigned int gasmix_idx = DC_GASMIX_UNKNOWN;
	unsigned int tank_id_previous = UNDEFINED;
	unsigned int tank_usage_previous = UNDEFINED;
	unsigned int tank_idx = UNDEFINED;
	unsigned int interval = 0;

	unsigned int have_time = 0;
	unsigned int have_depth = 0;
	unsigned int have_gasmix = 0;

	unsigned int time = 0;
	unsigned int offset = 0;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		unsigned int type = data[offset + 0];
		unsigned int length = data[offset + 1];

		if (length < 2 || offset + length > size) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}

		// Since log version 1.9, the ID_GF_INFO record has been deprecated and
		// replaced with the larger ID_GF_DATA record. Unfortunately some
		// earlier firmware versions produced records with the new type, but
		// with the old size. This has been fixed in log version 1.12.
		// Correct the record type to workaround this bug.
		if (type == ID_GF_DATA && length == lengths[ID_GF_INFO]) {
			type = ID_GF_INFO;
		}

		if (type < C_ARRAY_SIZE(lengths)) {
			if (length != lengths[type]) {
				ERROR (abstract->context, "Unexpected record size (%u %u).", length, lengths[type]);
				return DC_STATUS_DATAFORMAT;
			}
		}

		// Generate a timestamp for the first non-config record and every
		// depth record, except the first one. The first depth record must be
		// excluded because the sample already has a timestamp from the first
		// non-config record.
		if ((!have_time && !ISCONFIG(type)) ||
			(have_depth && type == ID_DEPTH)) {
			time += interval;
			sample.time = time * 1000;
			if (callback) callback(DC_SAMPLE_TIME, &sample, userdata);
			have_time = 1;
		}

		if (type == ID_LOG_VERSION) {
			logversion = array_uint16_be (data + offset + 2);
			unsigned int version_major = data[offset + 2];
			unsigned int version_minor = data[offset + 3];
			DEBUG (abstract->context, "Version: %u.%u",
				version_major, version_minor);
		} else if (type == ID_HEADER) {
			unsigned int model = data[offset + 2];
			unsigned int hw_major = data[offset + 3];
			unsigned int hw_minor = data[offset + 4];
			unsigned int fw_major = data[offset + 5];
			unsigned int fw_minor = data[offset + 6];
			unsigned int fw_bugfix = data[offset + 7];
			unsigned int deco_major = data[offset + 8];
			unsigned int deco_minor = data[offset + 9];
			interval = data[offset + 10];
			unsigned int DC_ATTR_UNUSED detection = data[offset + 11];
			unsigned int DC_ATTR_UNUSED noflytime = data[offset + 12];
			divemode = data[offset + 13];
			timezone = (signed char) data[offset + 14];
			atmospheric = array_uint16_le(data + offset + 16);
			unsigned int DC_ATTR_UNUSED number = array_uint16_le(data + offset + 18);
			unsigned int DC_ATTR_UNUSED battery = array_uint16_le(data + offset + 20);
			time_start = array_uint32_le(data + offset + 24);
			unsigned int serial = array_uint32_le(data + offset + 28);
			DEBUG (abstract->context, "Device: model=%u, hw=%u.%u, fw=%u.%u.%u, deco=%u.%u, serial=%u",
				model,
				hw_major, hw_minor,
				fw_major, fw_minor, fw_bugfix,
				deco_major, deco_minor,
				serial);
		} else if (type == ID_GAS_SWITCH) {
			unsigned int id = UNDEFINED;
			unsigned int o2 = data[offset + 2];
			unsigned int he = data[offset + 3];

			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (id == gasmix[idx].id &&
					o2 == gasmix[idx].oxygen &&
					he == gasmix[idx].helium)
					break;
				idx++;
			}
			if (idx >= ngasmixes) {
				if (ngasmixes >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_NOMEMORY;
				}
				gasmix[ngasmixes].id = id;
				gasmix[ngasmixes].oxygen = o2;
				gasmix[ngasmixes].helium = he;
				ngasmixes++;
			}
			sample.gasmix = idx;
			if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
		} else if (type == ID_DEPTH) {
			unsigned int depth = array_uint16_le (data + offset + 2);
			if (maxdepth < depth)
				maxdepth = depth;
			sample.depth = depth / 100.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			have_depth = 1;
		} else if (type == ID_TEMPERATURE) {
			unsigned int temperature = array_uint16_le (data + offset + 2);
			sample.depth = temperature / 10.0;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
		} else if (type == ID_OC_CC_SWITCH) {
			unsigned int DC_ATTR_UNUSED ccr = data[offset + 2];
		} else if (type == ID_GAS_TRANSMITTER) {
			unsigned int gas_id = data[offset + 2];
			unsigned int DC_ATTR_UNUSED battery = array_uint16_le (data + offset + 4);
			unsigned int pressure = array_uint16_le (data + offset + 6);
			unsigned int transmitter = array_uint16_le (data + offset + 8);
			dc_usage_t usage = DC_USAGE_NONE;

			if (have_gasmix && gasmix_id_previous != gas_id) {
				unsigned int idx = 0;
				while (idx < ngasmixes) {
					if (gas_id == gasmix[idx].id)
						break;
					idx++;
				}
				if (idx >= ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix id (%u).", gas_id);
					return DC_STATUS_DATAFORMAT;
				}
				sample.gasmix = idx;
				if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
				gasmix_id_previous = gas_id;
				gasmix_idx = idx;
			}

			if (tank_id_previous != transmitter ||
				tank_usage_previous != usage) {
				// Find the tank in the list.
				unsigned int idx = 0;
				while (idx < ntanks) {
					if (tank[idx].id == transmitter &&
						tank[idx].usage == usage)
						break;
					idx++;
				}

				// Add a new tank if necessary.
				if (idx >= ntanks) {
					if (ngasmixes >= NTANKS) {
						ERROR (abstract->context, "Maximum number of tanks reached.");
						return DC_STATUS_NOMEMORY;
					}
					tank[ntanks].id = transmitter;
					tank[ntanks].beginpressure = pressure;
					tank[ntanks].endpressure = pressure;
					tank[ntanks].gasmix = gasmix_idx;
					tank[ntanks].usage = usage;
					ntanks++;
				}

				tank_id_previous = transmitter;
				tank_usage_previous = usage;
				tank_idx = idx;
			}
			tank[tank_idx].endpressure = pressure;

			sample.pressure.tank = tank_idx;
			sample.pressure.value = pressure / 10.0;
			if (callback) callback(DC_SAMPLE_PRESSURE, &sample, userdata);
		} else if (type == ID_COMPARTMENTS) {
			for (unsigned int i = 0; i < 16; ++i) {
				unsigned int DC_ATTR_UNUSED n2 = array_uint16_le (data + offset +  4 + i * 2);
				unsigned int DC_ATTR_UNUSED he = array_uint16_le (data + offset + 36 + i * 2);
			}
		} else if (type == ID_GPS) {
			if (!have_location) {
				longitude = (signed int) array_uint32_le (data + offset + 4);
				latitude  = (signed int) array_uint32_le (data + offset + 8);
				have_location = 1;
			} else {
				WARNING (abstract->context, "Multiple GPS locations present.");
			}
		} else if (type == ID_PO2_BOARD) {
			unsigned int DC_ATTR_UNUSED serial = array_uint16_le (data + offset + 6);
			for (unsigned int i = 0; i < 3; ++i) {
				unsigned int ppo2 = data[offset + 2 + i];
				sample.ppo2.sensor = i;
				sample.ppo2.value = ppo2 / 100.0;
				if (callback) callback(DC_SAMPLE_PPO2, &sample, userdata);
			}
		} else if (type == ID_DECO) {
			unsigned int ndt = data[offset + 2];
			unsigned int ceiling = data[offset + 3];
			unsigned int cns = data[offset + 4];
			unsigned int DC_ATTR_UNUSED safetystop = data[offset + 5];
			unsigned int DC_ATTR_UNUSED ceiling_max = array_uint16_le (data + offset + 6);
			unsigned int tts = array_uint16_le (data + offset + 8);
			unsigned int DC_ATTR_UNUSED otu = array_uint16_le (data + offset + 10);

			// Deco / NDL
			if (ceiling) {
				sample.deco.type = DC_DECO_DECOSTOP;
				sample.deco.time = 0;
				sample.deco.depth = ceiling;
			} else {
				sample.deco.type = DC_DECO_NDL;
				sample.deco.time = ndt * 60;
				sample.deco.depth = 0.0;
			}
			sample.deco.tts = tts;
			if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);

			sample.cns = cns / 100.0;
			if (callback) callback(DC_SAMPLE_CNS, &sample, userdata);
		} else if (type == ID_GF) {
			if (gf_lo == UNDEFINED && gf_hi == UNDEFINED) {
				gf_lo = data[offset + 2];
				gf_hi = data[offset + 3];
			} else {
				WARNING (abstract->context, "Multiple GF values present.");
			}
		} else if (type == ID_FOOTER) {
			unsigned int DC_ATTR_UNUSED cns = data[offset + 2];
			unsigned int DC_ATTR_UNUSED violations = data[offset + 3];
			unsigned int DC_ATTR_UNUSED otu = array_uint16_le (data + offset + 4);
			unsigned int DC_ATTR_UNUSED battery = array_uint16_le (data + offset + 6);
			time_end = array_uint32_le(data + offset + 8);
			unsigned int DC_ATTR_UNUSED desaturation = array_uint32_le (data + offset + 12);
		} else if (type == ID_PO2_REBREATHER) {
			for (unsigned int i = 0; i < 3; ++i) {
				unsigned int ppo2 = data[offset + 2 + i];
				sample.ppo2.sensor = i;
				sample.ppo2.value = ppo2 / 100.0;
				if (callback) callback(DC_SAMPLE_PPO2, &sample, userdata);
			}
			unsigned int pressure = array_uint16_le (data + offset + 8);
			unsigned int serial   = array_uint16_le (data + offset + 10);
			dc_usage_t usage = DC_USAGE_OXYGEN;

			if (tank_id_previous != serial ||
				tank_usage_previous != usage) {
				// Find the tank in the list.
				unsigned int idx = 0;
				while (idx < ntanks) {
					if (tank[idx].id == serial &&
						tank[idx].usage == usage)
						break;
					idx++;
				}

				// Add a new tank if necessary.
				if (idx >= ntanks) {
					if (ngasmixes >= NTANKS) {
						ERROR (abstract->context, "Maximum number of tanks reached.");
						return DC_STATUS_NOMEMORY;
					}
					tank[ntanks].id = serial;
					tank[ntanks].beginpressure = pressure;
					tank[ntanks].endpressure = pressure;
					tank[ntanks].gasmix = DC_GASMIX_UNKNOWN;
					tank[ntanks].usage = usage;
					ntanks++;
				}

				tank_id_previous = serial;
				tank_usage_previous = usage;
				tank_idx = idx;
			}
			tank[tank_idx].endpressure = pressure;

			sample.pressure.tank = tank_idx;
			sample.pressure.value = pressure / 10.0;
			if (callback) callback(DC_SAMPLE_PRESSURE, &sample, userdata);
		} else if (type == ID_COMPASS) {
			unsigned int heading = array_uint16_le (data + offset + 4);
			sample.bearing = heading;
			if (callback) callback(DC_SAMPLE_BEARING, &sample, userdata);
		} else if (type == ID_TRIM) {
			int DC_ATTR_UNUSED trim = (signed int) data[offset + 2];
		} else if (type == ID_GAS_CONFIG) {
			unsigned int id = data[offset + 2];
			unsigned int o2 = data[offset + 3];
			unsigned int he = data[offset + 4];
			if (o2 != 0 || he != 0) {
				unsigned int idx = 0;
				while (idx < ngasmixes) {
					if (id == gasmix[idx].id)
						break;
					idx++;
				}
				if (idx >= ngasmixes) {
					if (ngasmixes >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					gasmix[ngasmixes].id = id;
					gasmix[ngasmixes].oxygen = o2;
					gasmix[ngasmixes].helium = he;
					ngasmixes++;
					have_gasmix = 1;
				} else {
					if (gasmix[idx].oxygen != o2 ||
						gasmix[idx].helium != he) {
						ERROR (abstract->context, "Gas mix %u changed (%u/%u -> %u/%u).",
							gasmix[idx].id,
							gasmix[idx].oxygen, gasmix[idx].helium,
							o2, he);
						return DC_STATUS_DATAFORMAT;
					}

					sample.gasmix = idx;
					if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
				}
			}
		} else if (type == ID_TANK_TRANSMITTER) {
			unsigned int id = data[offset + 2] | TRANSMITTER_ID;
			unsigned int DC_ATTR_UNUSED battery = array_uint16_le (data + offset + 4);
			unsigned int pressure = array_uint16_le (data + offset + 6) / 10;
			dc_usage_t usage = DC_USAGE_NONE;

			if (tank_id_previous != id ||
				tank_usage_previous != usage) {
				// Find the tank in the list.
				unsigned int idx = 0;
				while (idx < ntanks) {
					if (tank[idx].id == id &&
						tank[idx].usage == usage)
						break;
					idx++;
				}

				// Add a new tank if necessary.
				if (idx >= ntanks) {
					if (ngasmixes >= NTANKS) {
						ERROR (abstract->context, "Maximum number of tanks reached.");
						return DC_STATUS_NOMEMORY;
					}
					tank[ntanks].id = id;
					tank[ntanks].beginpressure = pressure;
					tank[ntanks].endpressure = pressure;
					tank[ntanks].gasmix = DC_GASMIX_UNKNOWN;
					tank[ntanks].usage = usage;
					ntanks++;
				}

				tank_id_previous = id;
				tank_usage_previous = usage;
				tank_idx = idx;
			}
			tank[tank_idx].endpressure = pressure;

			sample.pressure.tank = tank_idx;
			sample.pressure.value = pressure / 10.0;
			if (callback) callback(DC_SAMPLE_PRESSURE, &sample, userdata);
		} else if (type == ID_GF_INFO || type == ID_GF_DATA) {
			unsigned int DC_ATTR_UNUSED gf_now  = array_uint16_le (data + offset + 2);
			unsigned int DC_ATTR_UNUSED gf_surface  = array_uint16_le (data + offset + 4);
			if (type == ID_GF_DATA) {
				unsigned int DC_ATTR_UNUSED leading_tissue_gf_now = data[offset + 6];
				unsigned int DC_ATTR_UNUSED leading_tissue_gf_surface = data[offset + 7];
			}
		} else if (type == ID_SGC) {
			unsigned int DC_ATTR_UNUSED sgc = array_uint16_le (data + offset + 2);
		} else {
			WARNING (abstract->context, "Unknown record (type=%u, size=%u)", type, length);
		}

		offset += length;
	}

	parser->cached = 1;
	parser->logversion = logversion;
	parser->datetime = time_start;
	parser->timezone = timezone;
	if (time_start != UNDEFINED && time_end != UNDEFINED) {
		parser->divetime = time_end - time_start;
	} else {
		parser->divetime = time;
	}
	parser->maxdepth = maxdepth;
	parser->divemode = divemode;
	parser->atmospheric = atmospheric;
	parser->gf_lo = gf_lo;
	parser->gf_hi = gf_hi;
	parser->have_location = have_location;
	parser->latitude = latitude;
	parser->longitude = longitude;
	parser->ngasmixes = ngasmixes;
	parser->ntanks = ntanks;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i] = tank[i];
	}

	return DC_STATUS_SUCCESS;
}
