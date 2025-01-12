/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Jef Driesen
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

#include "liquivision_lynx.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &liquivision_lynx_parser_vtable)

#define XEN  0
#define XEO  1
#define LYNX 2
#define KAON 3

#define XEN_V1   0x83321485 // Not supported
#define XEN_V2   0x83321502
#define XEN_V3   0x83328401

#define XEO_V1_A 0x17485623
#define XEO_V1_B 0x27485623
#define XEO_V2_A 0x17488401
#define XEO_V2_B 0x27488401
#define XEO_V3_A 0x17488402
#define XEO_V3_B 0x27488402

#define LYNX_V1  0x67488403
#define LYNX_V2  0x67488404
#define LYNX_V3  0x67488405

#define KAON_V1  0x37488402
#define KAON_V2  0x47488402

#define SZ_HEADER_XEN   80
#define SZ_HEADER_OTHER 96

#define FRESH    0
#define BRACKISH 1
#define SALT     2

#define DECO  0
#define GAUGE 1
#define TEC   2
#define REC   3

#define ZHL16GF 0
#define RGBM    1

#define NORMAL                 0
#define BOOKMARK               1
#define ALARM_DEPTH            2
#define ALARM_TIME             3
#define ALARM_VELOCITY         4
#define DECOSTOP               5
#define DECOSTOP_BREACHED      6
#define GASMIX                 7
#define SETPOINT               8
#define BAILOUT_ON             9
#define BAILOUT_OFF           10
#define EMERGENCY_ON          11
#define EMERGENCY_OFF         12
#define LOST_GAS              13
#define SAFETY_STOP           14
#define TANK_PRESSURE         15
#define TANK_LIST             16

#define NGASMIXES 11
#define NTANKS    11

#define INVALID 0xFFFFFFFF

typedef struct liquivision_lynx_parser_t liquivision_lynx_parser_t;

typedef struct liquivision_lynx_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} liquivision_lynx_gasmix_t;

typedef struct liquivision_lynx_tank_t {
	unsigned int id;
	unsigned int beginpressure;
	unsigned int endpressure;
} liquivision_lynx_tank_t;

struct liquivision_lynx_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int headersize;
	// Cached fields.
	unsigned int cached;
	unsigned int ngasmixes;
	unsigned int ntanks;
	liquivision_lynx_gasmix_t gasmix[NGASMIXES];
	liquivision_lynx_tank_t tank[NTANKS];
};

static dc_status_t liquivision_lynx_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t liquivision_lynx_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t liquivision_lynx_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t liquivision_lynx_parser_vtable = {
	sizeof(liquivision_lynx_parser_t),
	DC_FAMILY_LIQUIVISION_LYNX,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	liquivision_lynx_parser_get_datetime, /* datetime */
	liquivision_lynx_parser_get_field, /* fields */
	liquivision_lynx_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
liquivision_lynx_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	liquivision_lynx_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (liquivision_lynx_parser_t *) dc_parser_allocate (context, &liquivision_lynx_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->headersize = (model == XEN) ? SZ_HEADER_XEN : SZ_HEADER_OTHER;
	parser->cached = 0;
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

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
liquivision_lynx_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	liquivision_lynx_parser_t *parser = (liquivision_lynx_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + 40;

	if (datetime) {
		datetime->year   = array_uint16_le (p + 18);
		datetime->month  = array_uint16_le (p + 16) + 1;
		datetime->day    = array_uint16_le (p + 12) + 1;
		datetime->hour   = array_uint16_le (p + 8);
		datetime->minute = array_uint16_le (p + 6);
		datetime->second = array_uint16_le (p + 4);
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
liquivision_lynx_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	liquivision_lynx_parser_t *parser = (liquivision_lynx_parser_t *) abstract;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		dc_status_t rc = liquivision_lynx_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint32_le (abstract->data + 4);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (abstract->data + 28) / 100.0;
			break;
		case DC_FIELD_AVGDEPTH:
			*((double *) value) = array_uint16_le (abstract->data + 30) / 100.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed short) array_uint16_le (abstract->data + 34) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = (signed short) array_uint16_le (abstract->data + 36) / 10.0;
			break;
		case DC_FIELD_SALINITY:
			switch (abstract->data[38]) {
			case FRESH:
				water->type = DC_WATER_FRESH;
				water->density = 1000.0;
				break;
			case BRACKISH:
				water->type = DC_WATER_SALT;
				water->density = 1015.0;
				break;
			case SALT:
				water->type = DC_WATER_SALT;
				water->density = 1025.0;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_le (abstract->data + 26) / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (parser->model == XEN) {
				*((unsigned int *) value) = DC_DIVEMODE_GAUGE;
			} else {
				switch (abstract->data[92] & 0x0F) {
				case DECO:
				case TEC:
				case REC:
					*((unsigned int *) value) = DC_DIVEMODE_OC;
					break;
				case GAUGE:
					*((unsigned int *) value) = DC_DIVEMODE_GAUGE;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			}
			break;
		case DC_FIELD_DECOMODEL:
			switch (abstract->data[93]) {
			case ZHL16GF:
				decomodel->type = DC_DECOMODEL_BUHLMANN;
				decomodel->conservatism = 0;
				decomodel->params.gf.low  = 0;
				decomodel->params.gf.high = 0;
				break;
			case RGBM:
				decomodel->type = DC_DECOMODEL_RGBM;
				decomodel->conservatism = 0;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
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
			tank->beginpressure = parser->tank[flags].beginpressure / 100.0;
			tank->endpressure   = parser->tank[flags].endpressure / 100.0;
			tank->gasmix = DC_GASMIX_UNKNOWN;
			tank->usage = DC_USAGE_NONE;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	liquivision_lynx_parser_t *parser = (liquivision_lynx_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	// Get the version.
	unsigned int version = array_uint32_le(data);

	// Get the sample interval.
	unsigned int interval_idx = data[39];
	const unsigned int intervals[] = {1, 2, 5, 10, 30, 60};
	if (interval_idx >= C_ARRAY_SIZE(intervals)) {
		ERROR (abstract->context, "Invalid sample interval index %u", interval_idx);
		return DC_STATUS_DATAFORMAT;
	}
	unsigned int interval = intervals[interval_idx];

	// Get the number of samples and events.
	unsigned int nsamples = array_uint32_le (data + 8);
	unsigned int nevents = array_uint32_le (data + 12);

	unsigned int ngasmixes = 0;
	unsigned int ntanks = 0;
	liquivision_lynx_gasmix_t gasmix[NGASMIXES] = {0};
	liquivision_lynx_tank_t tank[NTANKS] = {0};
	unsigned int o2_previous = INVALID, he_previous = INVALID;
	unsigned int gasmix_idx = INVALID;
	unsigned int have_gasmix = 0;
	unsigned int tank_id_previous = INVALID;
	unsigned int tank_idx = INVALID;
	unsigned int pressure[NTANKS] = {0};
	unsigned int have_pressure = 0;
	unsigned int setpoint = 0, have_setpoint = 0;
	unsigned int deco = 0, have_deco = 0;

	unsigned int time = 0;
	unsigned int samples = 0;
	unsigned int events = 0;
	unsigned int offset = parser->headersize;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		unsigned int value = array_uint16_le (data + offset);
		offset += 2;

		if (value & 0x8000) {
			if (events >= nevents) {
				break;
			}

			if (offset + 4 > size) {
				ERROR (abstract->context, "Buffer overflow at offset %u", offset);
				return DC_STATUS_DATAFORMAT;
			}

			unsigned int type = value & 0x7FFF;
			unsigned int DC_ATTR_UNUSED timestamp = array_uint32_le (data + offset + 2);
			offset += 4;

			// Get the sample length.
			unsigned int length = 0;
			switch (type) {
			case DECOSTOP:
			case GASMIX:
				length = 2;
				break;
			case SETPOINT:
				length = 1;
				break;
			case TANK_LIST:
				length = NTANKS * 2;
				break;
			case TANK_PRESSURE:
				if (version == LYNX_V1) {
					length = 4;
				} else {
					length = 6;
				}
				break;
			default:
				break;
			}

			if (offset + length > size) {
				ERROR (abstract->context, "Buffer overflow at offset %u", offset);
				return DC_STATUS_DATAFORMAT;
			}

			unsigned int o2 = 0, he = 0;
			unsigned int tank_id = 0, tank_pressure = 0;

			switch (type) {
			case NORMAL:
			case BOOKMARK:
			case ALARM_DEPTH:
			case ALARM_TIME:
			case ALARM_VELOCITY:
			case DECOSTOP_BREACHED:
			case BAILOUT_ON:
			case BAILOUT_OFF:
			case EMERGENCY_ON:
			case EMERGENCY_OFF:
			case LOST_GAS:
			case SAFETY_STOP:
				break;
			case DECOSTOP:
				deco = array_uint16_le (data + offset);
				have_deco = 1;
				break;
			case GASMIX:
				o2 = data[offset + 0];
				he = data[offset + 1];
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

					o2_previous = o2;
					he_previous = he;
					gasmix_idx = i;
					have_gasmix = 1;
				}
				break;
			case SETPOINT:
				setpoint = data[offset];
				have_setpoint = 1;
				break;
			case TANK_PRESSURE:
				tank_id       = array_uint16_le (data + offset + 0);
				tank_pressure = array_uint16_le (data + offset + 2);
				if (tank_id != tank_id_previous) {
					// Find the tank in the list.
					unsigned int i = 0;
					while (i < ntanks) {
						if (tank_id == tank[i].id)
							break;
						i++;
					}

					// Add a new tank if necessary.
					if (i >= ntanks) {
						if (i >= NTANKS) {
							ERROR (abstract->context, "Maximum number of tanks reached.");
							return DC_STATUS_DATAFORMAT;
						}
						tank[i].id = tank_id;
						tank[i].beginpressure = tank_pressure;
						tank[i].endpressure = tank_pressure;
						ntanks = i + 1;
					}

					tank_id_previous = tank_id;
					tank_idx = i;
				}
				tank[tank_idx].endpressure = tank_pressure;
				pressure[tank_idx] = tank_pressure;
				have_pressure |= 1 << tank_idx;
				break;
			case TANK_LIST:
				break;
			default:
				WARNING (abstract->context, "Unknown event %u", type);
				break;
			}

			offset += length;
			events++;
		} else {
			if (samples >= nsamples) {
				break;
			}

			// Get the sample length.
			unsigned int length = 2;
			if (version == XEO_V1_A || version == XEO_V2_A ||
				version == XEO_V3_A || version == KAON_V1 ||
				version == KAON_V2) {
				length += 14;
			}

			if (offset + length > size) {
				ERROR (abstract->context, "Buffer overflow at offset %u", offset);
				return DC_STATUS_DATAFORMAT;
			}

			// Time (seconds).
			time += interval;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Depth (1/100 m).
			sample.depth = value / 100.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Temperature (1/10 °C).
			int temperature = (signed short) array_uint16_le (data + offset);
			sample.temperature = temperature / 10.0;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

			// Gas mix
			if (have_gasmix) {
				sample.gasmix = gasmix_idx;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				have_gasmix = 0;
			}

			// Setpoint (1/10 bar).
			if (have_setpoint) {
				sample.setpoint = setpoint / 10.0;
				if (callback) callback (DC_SAMPLE_SETPOINT, &sample, userdata);
				have_setpoint = 0;
			}

			// Tank pressure (1/100 bar).
			if (have_pressure) {
				for (unsigned int i = 0; i < ntanks; ++i) {
					if (have_pressure & (1 << i)) {
						sample.pressure.tank = i;
						sample.pressure.value = pressure[i] / 100.0;
						if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
					}
				}
				have_pressure = 0;
			}

			// Deco/ndl
			if (have_deco) {
				if (deco) {
					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.depth = deco / 100.0;
				} else {
					sample.deco.type = DC_DECO_NDL;
					sample.deco.depth = 0.0;
				}
				sample.deco.time = 0;
				sample.deco.tts = 0;
				if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
				have_deco = 0;
			}

			offset += length;
			samples++;
		}
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
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
