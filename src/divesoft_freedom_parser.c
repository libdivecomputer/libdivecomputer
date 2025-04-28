/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Jan Matoušek, Jef Driesen
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

#include "divesoft_freedom.h"
#include "context-private.h"
#include "parser-private.h"
#include "checksum.h"
#include "array.h"

#define UNDEFINED 0xFFFFFFFF

#define EPOCH 946684800 // 2000-01-01 00:00:00 UTC

#define OC      0
#define OXYGEN  1
#define DILUENT 2

#define NSENSORS  4
#define NGASMIXES 12
#define NTANKS    12

#define HEADER_SIGNATURE_V1 0x45766944 // "DivE"
#define HEADER_SIGNATURE_V2 0x45566944 // "DiVE"

#define HEADER_SIZE_V1 32
#define HEADER_SIZE_V2 64

#define RECORD_SIZE 16

#define SEAWATER   1028
#define FRESHWATER 1000

typedef enum logrecord_t {
	LREC_POINT          = 0,
	LREC_MANIPULATION   = 1,
	LREC_AUTO           = 2,
	LREC_DIVER_ERROR    = 3,
	LREC_INTERNAL_ERROR = 4,
	LREC_ACTIVITY       = 5,
	LREC_CONFIGURATION  = 6,
	LREC_MEASURE        = 7,
	LREC_STATE          = 8,
	LREC_INFO           = 9,
} logrecord_t;

typedef enum point_id_t {
	POINT_1     = 0,
	POINT_2     = 1,
	POINT_1_OLD = 0x3FF,
} point_id_t;

typedef enum configuration_id_t {
	CFG_ID_TEST_CCR_FULL      = 0,
	CFG_ID_TEST_CCR_PARTIAL   = 1,
	CFG_ID_OXYGEN_CALIBRATION = 2,
	CFG_ID_SERIAL             = 3,
	CFG_ID_DECO               = 4,
	CFG_ID_VERSION            = 5,
	CFG_ID_ASCENT             = 6,
	CFG_ID_AI                 = 7,
	CFG_ID_CCR                = 8,
	CFG_ID_DILUENTS           = 9,
} configuration_id_t;

typedef enum measure_id_t {
	MEASURE_ID_OXYGEN      = 0,
	MEASURE_ID_BATTERY     = 1,
	MEASURE_ID_HELIUM      = 2,
	MEASURE_ID_OXYGEN_MV   = 3,
	MEASURE_ID_GPS         = 4,
	MEASURE_ID_PRESSURE    = 5,
	MEASURE_ID_AI_SAC      = 6,
	MEASURE_ID_AI_PRESSURE = 7,
	MEASURE_ID_BRIGHTNESS  = 8,
	MEASURE_ID_AI_STAT     = 9,
} measure_id_t;

typedef enum state_id_t {
	STATE_ID_DECO_N2LOW  = 0,
	STATE_ID_DECO_N2HIGH = 1,
	STATE_ID_DECO_HELOW  = 2,
	STATE_ID_DECO_HEHIGH = 3,
	STATE_ID_PLAN_STEPS  = 4,
} state_id_t;

typedef enum event_t {
	EVENT_DUMMY                = 0,
	EVENT_SETPOINT_MANUAL      = 1,
	EVENT_SETPOINT_AUTO        = 2,
	EVENT_OC                   = 3,
	EVENT_CCR                  = 4,
	EVENT_MIX_CHANGED          = 5,
	EVENT_START                = 6,
	EVENT_TOO_FAST             = 7,
	EVENT_ABOVE_CEILING        = 8,
	EVENT_TOXIC                = 9,
	EVENT_HYPOX                = 10,
	EVENT_CRITICAL             = 11,
	EVENT_SENSOR_DISABLED      = 12,
	EVENT_SENSOR_ENABLED       = 13,
	EVENT_O2_BACKUP            = 14,
	EVENT_PEER_DOWN            = 15,
	EVENT_HS_DOWN              = 16,
	EVENT_INCONSISTENT         = 17,
	EVENT_KEYDOWN              = 18,
	EVENT_SCR                  = 19,
	EVENT_ABOVE_STOP           = 20,
	EVENT_SAFETY_MISS          = 21,
	EVENT_FATAL                = 22,
	EVENT_DILUENT              = 23,
	EVENT_CHANGE_MODE          = 24,
	EVENT_SOLENOID             = 25,
	EVENT_BOOKMARK             = 26,
	EVENT_GF_SWITCH            = 27,
	EVENT_PEER_UP              = 28,
	EVENT_HS_UP                = 29,
	EVENT_CNS                  = 30,
	EVENT_BATTERY_LOW          = 31,
	EVENT_PPO2_LOST            = 32,
	EVENT_SENSOR_VALUE_BAD     = 33,
	EVENT_SAFETY_STOP_END      = 34,
	EVENT_DECO_STOP_END        = 35,
	EVENT_DEEP_STOP_END        = 36,
	EVENT_NODECO_END           = 37,
	EVENT_DEPTH_REACHED        = 38,
	EVENT_TIME_ELAPSED         = 39,
	EVENT_STACK_USAGE          = 40,
	EVENT_GAS_SWITCH_INFO      = 41,
	EVENT_PRESSURE_SENS_WARN   = 42,
	EVENT_PRESSURE_SENS_FAIL   = 43,
	EVENT_CHECK_O2_SENSORS     = 44,
	EVENT_SWITCH_TO_COMP_SCR   = 45,
	EVENT_GAS_LOST             = 46,
	EVENT_AIRBREAK             = 47,
	EVENT_AIRBREAK_END         = 48,
	EVENT_AIRBREAK_MISSED      = 49,
	EVENT_BORMT_EXPIRATION     = 50,
	EVENT_BORMT_EXPIRED        = 51,
	EVENT_SENSOR_EXCLUDED      = 52,
	EVENT_PREBR_SKIPPED        = 53,
	EVENT_BOCCR_BORMT_EXPIRED  = 54,
	EVENT_WAYPOINT             = 55,
	EVENT_TURNAROUND           = 56,
	EVENT_SOLENOID_FAILURE     = 57,
	EVENT_SM_CYL_PRESS_DIFF    = 58,
	EVENT_BAILOUT_MOD_EXCEEDED = 59,
} event_t;

typedef enum divemode_t {
	STMODE_UNKNOWN = 0,
	STMODE_OC      = 1,
	STMODE_CCR     = 2,
	STMODE_MCCR    = 3,
	STMODE_FREE    = 4,
	STMODE_GAUGE   = 5,
	STMODE_ASCR    = 6,
	STMODE_PSCR    = 7,
	STMODE_BOCCR   = 8,
} divemode_t;

typedef enum setpoint_change_t {
	SP_MANUAL          = 0,
	SP_AUTO_START      = 1,
	SP_AUTO_HYPOX      = 2,
	SP_AUTO_TIMEOUT    = 3,
	SP_AUTO_ASCENT     = 4,
	SP_AUTO_STALL      = 5,
	SP_AUTO_SPLOW      = 6,
	SP_AUTO_DEPTH_DESC = 7,
	SP_AUTO_DEPTH_ASC  = 8,
} setpoint_change_t;

typedef enum sensor_state_t {
	SENSTAT_NORMAL       = 0,
	SENSTAT_OVERRANGE    = 1,
	SENSTAT_DISABLED     = 2,
	SENSTAT_EXCLUDED     = 3,
	SENSTAT_UNCALIBRATED = 4,
	SENSTAT_ERROR        = 5,
	SENSTAT_OFFLINE      = 6,
	SENSTAT_INHIBITED    = 7,
	SENSTAT_NOT_EXIST    = 8,
} sensor_state_t;

typedef enum battery_state_t {
	BATSTATE_NO_BATTERY  = 0,
	BATSTATE_UNKNOWN     = 1,
	BATSTATE_DISCHARGING = 2,
	BATSTATE_CHARGING    = 3,
	BATSTATE_FULL        = 4,
} battery_state_t;

typedef struct divesoft_freedom_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
	unsigned int type;
	unsigned int id;
} divesoft_freedom_gasmix_t;

typedef struct divesoft_freedom_tank_t {
	unsigned int volume;
	unsigned int workpressure;
	unsigned int beginpressure;
	unsigned int endpressure;
	unsigned int transmitter;
	unsigned int active;
} divesoft_freedom_tank_t;

typedef struct divesoft_freedom_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int version;
	unsigned int headersize;
	unsigned int divetime;
	unsigned int divemode;
	int temperature_min;
	unsigned int maxdepth;
	unsigned int atmospheric;
	unsigned int avgdepth;
	unsigned int ngasmixes;
	divesoft_freedom_gasmix_t gasmix[NGASMIXES];
	unsigned int diluent;
	unsigned int ntanks;
	divesoft_freedom_tank_t tank[NTANKS];
	unsigned int vpm;
	unsigned int gf_lo;
	unsigned int gf_hi;
	unsigned int seawater;
	unsigned int calibration[NSENSORS];
	unsigned int calibrated;
	unsigned int have_location;
	int latitude;
	int longitude;
} divesoft_freedom_parser_t;

static dc_status_t divesoft_freedom_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t divesoft_freedom_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t divesoft_freedom_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t divesoft_freedom_parser_vtable = {
	sizeof(divesoft_freedom_parser_t),
	DC_FAMILY_DIVESOFT_FREEDOM,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	divesoft_freedom_parser_get_datetime, /* datetime */
	divesoft_freedom_parser_get_field, /* fields */
	divesoft_freedom_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static unsigned int
divesoft_freedom_find_gasmix (divesoft_freedom_gasmix_t gasmix[], unsigned int count, unsigned int oxygen, unsigned int helium, unsigned int type)
{
	unsigned int i = 0;
	while (i < count) {
		if (oxygen == gasmix[i].oxygen &&
			helium == gasmix[i].helium &&
			type == gasmix[i].type)
			break;
		i++;
	}

	return i;
}

static unsigned int
divesoft_freedom_find_tank (divesoft_freedom_tank_t tank[], unsigned int count, unsigned int transmitter)
{
	unsigned int i = 0;
	while (i < count) {
		if (transmitter == tank[i].transmitter)
			break;
		i++;
	}

	return i;
}

static unsigned int
divesoft_freedom_is_ccr (unsigned int divemode)
{
	return divemode == STMODE_CCR || divemode == STMODE_MCCR ||
		divemode == STMODE_ASCR || divemode == STMODE_PSCR ||
		divemode == STMODE_BOCCR;
}

static dc_status_t
divesoft_freedom_cache (divesoft_freedom_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	unsigned int headersize = 4;
	if (size < headersize) {
		ERROR (abstract->context, "Unexpected header size (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int version = array_uint32_le (data);
	if (version == HEADER_SIGNATURE_V1) {
		headersize = HEADER_SIZE_V1;
	} else if (version == HEADER_SIGNATURE_V2) {
		headersize = HEADER_SIZE_V2;
	} else {
		ERROR (abstract->context, "Unexpected header version (%08x).", version);
		return DC_STATUS_DATAFORMAT;
	}

	if (size < headersize) {
		ERROR (abstract->context, "Unexpected header size (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned short crc = array_uint16_le (data + 4);
	unsigned short ccrc = checksum_crc16r_ansi (data + 6, headersize - 6, 0xFFFF, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Invalid header checksum (%04x %04x).", crc, ccrc);
		return DC_STATUS_DATAFORMAT;
	}

	// Parse the dive header.
	unsigned int divetime = 0;
	unsigned int divemode = 0;
	unsigned int temperature_min = 0;
	unsigned int maxdepth = 0;
	unsigned int atmospheric = 0;
	unsigned int avgdepth = 0;
	unsigned int diluent_o2 = 0, diluent_he = 0;
	if (version == HEADER_SIGNATURE_V1) {
		unsigned int misc1 = array_uint32_le (data + 12);
		unsigned int misc2 = array_uint32_le (data + 16);
		divetime = misc1 & 0x1FFFF;
		divemode = (misc1 & 0x38000000) >> 27;
		temperature_min = (signed int) signextend ((misc2 & 0xFFC0000) >> 18, 10);
		maxdepth = array_uint16_le (data + 20);
		atmospheric = array_uint16_le (data + 24);
		avgdepth = 0;
		diluent_o2 = data[26];
		diluent_he = data[27];
	} else {
		divetime = array_uint32_le (data + 12);
		divemode = data[18];
		temperature_min = (signed short) array_uint16_le (data + 24);
		maxdepth = array_uint16_le (data + 28);
		atmospheric = array_uint16_le (data + 32);
		avgdepth = array_uint16_le (data + 38);
		diluent_o2 = 0;
		diluent_he = 0;

		DEBUG (abstract->context, "Device: serial=%.4s-%.8s",
			data + 52, data + 56);
	}

	divesoft_freedom_gasmix_t gasmix_ai[NGASMIXES] = {0},
		gasmix_diluent[NGASMIXES] = {0},
		gasmix_event[NGASMIXES] = {0};
	unsigned int ngasmix_ai = 0,
		ngasmix_diluent = 0,
		ngasmix_event = 0;
	divesoft_freedom_tank_t tank[NTANKS] = {0};
	unsigned int ntanks = 0;

	unsigned int vpm = 0, gf_lo = 0, gf_hi = 0;
	unsigned int seawater = 0;
	unsigned int calibration[NSENSORS] = {0};
	unsigned int calibrated = 0;

	unsigned int gasmixid_previous = UNDEFINED;

	unsigned int have_location = 0;
	int latitude = 0;
	int longitude = 0;

	// Parse the dive profile.
	unsigned int offset = headersize;
	while (offset + RECORD_SIZE <= size) {
		if (array_isequal(data + offset, RECORD_SIZE, 0xFF)) {
			WARNING (abstract->context, "Skipping empty sample.");
			offset += RECORD_SIZE;
			continue;
		}

		unsigned int flags = array_uint32_le (data + offset);
		unsigned int type      = (flags & 0x0000000F) >> 0;
		unsigned int id        = (flags & 0x7FE00000) >> 21;

		if (type == LREC_CONFIGURATION) {
			// Configuration record.
			if (id == CFG_ID_DECO) {
				unsigned int misc = array_uint16_le (data + offset + 4);
				gf_lo = data[offset + 8];
				gf_hi = data[offset + 9];
				seawater = misc & 0x02;
				vpm      = misc & 0x20;
			} else if (id == CFG_ID_VERSION) {
				DEBUG (abstract->context, "Device: type=%u, hw=%u.%u, sw=%u.%u.%u.%u flags=%u",
					data[offset + 4],
					data[offset + 5], data[offset + 6],
					data[offset + 7], data[offset + 8], data[offset + 9],
					array_uint32_le (data + offset + 12),
					array_uint16_le (data + offset + 10));
			} else if (id == CFG_ID_SERIAL) {
				DEBUG (abstract->context, "Device: serial=%.4s-%.8s",
					data + offset + 4, data + offset + 8);
			} else if (id == CFG_ID_DILUENTS) {
				for (unsigned int i = 0; i < 4; ++i) {
					unsigned int o2 = data[offset + 4 + i * 3 + 0];
					unsigned int he = data[offset + 4 + i * 3 + 1];
					unsigned int state = data[offset + 4 + i * 3 + 2];
					if (state & 0x01) {
						if (ngasmix_diluent >= NGASMIXES) {
							ERROR (abstract->context, "Maximum number of gas mixes reached.");
							return DC_STATUS_NOMEMORY;
						}
						gasmix_diluent[ngasmix_diluent].oxygen = o2;
						gasmix_diluent[ngasmix_diluent].helium = he;
						gasmix_diluent[ngasmix_diluent].type = DILUENT;
						gasmix_diluent[ngasmix_diluent].id = (state & 0xFE) >> 1;
						ngasmix_diluent++;
					}
				}
			} else if (id == CFG_ID_OXYGEN_CALIBRATION) {
				for (unsigned int i = 0; i < NSENSORS; ++i) {
					calibration[i] = array_uint16_le (data + offset + 4 + i * 2);
				}
				calibrated = 1;
			} else if (id == CFG_ID_AI) {
				unsigned int o2 = data[offset + 4];
				unsigned int he = data[offset + 5];
				unsigned int volume = array_uint16_le (data + offset + 6);
				unsigned int workpressure = array_uint16_le (data + offset + 8);
				unsigned int transmitter = data[offset + 10];
				unsigned int gasmixid = data[offset + 11];

				// Workaround for a bug in some pre-release firmware versions,
				// where the ID of the CCR gas mixes (oxygen and diluent) is
				// not stored correctly.
				if (gasmixid < 10 && gasmixid <= gasmixid_previous && gasmixid_previous != UNDEFINED) {
					WARNING (abstract->context, "Fixed the CCR gas mix id (%u -> %u) for tank %u.",
						gasmixid, gasmixid + 10, ntanks);
					gasmixid += 10;
				}
				gasmixid_previous = gasmixid;

				// Add the gas mix.
				if (ngasmix_ai >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_NOMEMORY;
				}
				gasmix_ai[ngasmix_ai].oxygen = o2;
				gasmix_ai[ngasmix_ai].helium = he;
				if (gasmixid == 10) {
					gasmix_ai[ngasmix_ai].type = OXYGEN;
				} else if (gasmixid == 11) {
					gasmix_ai[ngasmix_ai].type = DILUENT;
				} else {
					gasmix_ai[ngasmix_ai].type = OC;
				}
				gasmix_ai[ngasmix_ai].id = gasmixid;
				ngasmix_ai++;

				// Add the tank.
				if (ntanks >= NTANKS) {
					ERROR (abstract->context, "Maximum number of tanks reached.");
					return DC_STATUS_NOMEMORY;
				}
				tank[ntanks].volume = volume;
				tank[ntanks].workpressure = workpressure;
				tank[ntanks].transmitter = transmitter;
				ntanks++;
			}
		} else if ((type >= LREC_MANIPULATION && type <= LREC_ACTIVITY) || type == LREC_INFO) {
			// Event record.
			unsigned int event = array_uint16_le (data + offset + 4);

			if (event == EVENT_MIX_CHANGED || event == EVENT_DILUENT || event == EVENT_CHANGE_MODE) {
				unsigned int o2 = data[offset + 6];
				unsigned int he = data[offset + 7];
				unsigned int mixtype = OC;
				if (event == EVENT_DILUENT) {
					mixtype = DILUENT;
				} else if (event == EVENT_CHANGE_MODE) {
					unsigned int mode = data[offset + 8];
					if (divesoft_freedom_is_ccr (mode)) {
						mixtype = DILUENT;
					}
				}

				unsigned int idx = divesoft_freedom_find_gasmix (gasmix_event, ngasmix_event, o2, he, mixtype);
				if (idx >= ngasmix_event) {
					if (ngasmix_event >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					gasmix_event[ngasmix_event].oxygen = o2;
					gasmix_event[ngasmix_event].helium = he;
					gasmix_event[ngasmix_event].type = mixtype;
					gasmix_event[ngasmix_event].id = UNDEFINED;
					ngasmix_event++;
				}
			}
		} else if (type == LREC_MEASURE) {
			// Measurement record.
			if (id == MEASURE_ID_AI_PRESSURE) {
				for (unsigned int i = 0; i < NTANKS; ++i) {
					unsigned int pressure = data[offset + 4 + i];
					if (pressure == 0 || pressure == 0xFF)
						continue;

					unsigned int idx = divesoft_freedom_find_tank (tank, ntanks, i);
					if (idx >= ntanks) {
						ERROR (abstract->context, "Tank %u not found.", idx);
						return DC_STATUS_DATAFORMAT;
					}

					if (!tank[idx].active) {
						tank[idx].active = 1;
						tank[idx].beginpressure = pressure;
						tank[idx].endpressure = pressure;
					}
					tank[idx].endpressure = pressure;
				}
			} else if (id == MEASURE_ID_GPS) {
				if (!have_location) {
					latitude  = (signed int) array_uint32_le (data + offset + 4);
					longitude = (signed int) array_uint32_le (data + offset + 8);
					have_location = 1;
				} else {
					WARNING (abstract->context, "Multiple GPS locations present.");
				}
			}
		}

		offset += RECORD_SIZE;
	}

	unsigned int ngasmixes = 0;
	divesoft_freedom_gasmix_t gasmix[NGASMIXES] = {0};
	unsigned int diluent = UNDEFINED;

	// Add the gas mixes from the AI integration records.
	for (unsigned int i = 0; i < ngasmix_ai; ++i) {
		gasmix[ngasmixes] = gasmix_ai[i];
		ngasmixes++;
	}

	// Add the gas mixes from the diluent records.
	for (unsigned int i = 0; i < ngasmix_diluent; ++i) {
		unsigned int idx = divesoft_freedom_find_gasmix (gasmix, ngasmixes,
			gasmix_diluent[i].oxygen, gasmix_diluent[i].helium, gasmix_diluent[i].type);
		if (idx >= ngasmixes) {
			if (ngasmixes >= NGASMIXES) {
				ERROR (abstract->context, "Maximum number of gas mixes reached.");
				return DC_STATUS_NOMEMORY;
			}
			gasmix[ngasmixes] = gasmix_diluent[i];
			ngasmixes++;
		}
	}

	// Add the initial diluent.
	if (divesoft_freedom_is_ccr (divemode) &&
		(diluent_o2 != 0 || diluent_he != 0)) {
		unsigned int idx = divesoft_freedom_find_gasmix (gasmix, ngasmixes,
			diluent_o2, diluent_he, DILUENT);
		if (idx >= ngasmixes) {
			if (ngasmixes >= NGASMIXES) {
				ERROR (abstract->context, "Maximum number of gas mixes reached.");
				return DC_STATUS_NOMEMORY;
			}
			gasmix[ngasmixes].oxygen = diluent_o2;
			gasmix[ngasmixes].helium = diluent_he;
			gasmix[ngasmixes].type = DILUENT;
			gasmix[ngasmixes].id = UNDEFINED;
			ngasmixes++;
		}

		// Index of the initial diluent.
		diluent = idx;
	}

	// Add the gas mixes from the gas change events.
	for (unsigned int i = 0; i < ngasmix_event; ++i) {
		unsigned int idx = divesoft_freedom_find_gasmix (gasmix, ngasmixes,
			gasmix_event[i].oxygen, gasmix_event[i].helium, gasmix_event[i].type);
		if (idx >= ngasmixes) {
			if (ngasmixes >= NGASMIXES) {
				ERROR (abstract->context, "Maximum number of gas mixes reached.");
				return DC_STATUS_NOMEMORY;
			}
			gasmix[ngasmixes] = gasmix_event[i];
			ngasmixes++;
		}
	}

	// Cache the data for later use.
	parser->cached = 1;
	parser->version = version;
	parser->headersize = headersize;
	parser->divetime = divetime;
	parser->divemode = divemode;
	parser->temperature_min = temperature_min;
	parser->maxdepth = maxdepth;
	parser->atmospheric = atmospheric;
	parser->avgdepth = avgdepth;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->diluent = diluent;
	parser->ntanks = ntanks;
	for (unsigned int i = 0; i < ntanks; ++i) {
		parser->tank[i] = tank[i];
	}
	parser->vpm = vpm;
	parser->gf_lo = gf_lo;
	parser->gf_hi = gf_hi;
	parser->seawater = seawater;
	for (unsigned int i = 0; i < NSENSORS; ++i) {
		parser->calibration[i] = calibration[i];
	}
	parser->calibrated = calibrated;
	parser->have_location = have_location;
	parser->latitude = latitude;
	parser->longitude = longitude;

	return DC_STATUS_SUCCESS;
}

dc_status_t
divesoft_freedom_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	divesoft_freedom_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (divesoft_freedom_parser_t *) dc_parser_allocate (context, &divesoft_freedom_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->version = 0;
	parser->headersize = 0;
	parser->divetime = 0;
	parser->divemode = 0;
	parser->temperature_min = 0;
	parser->maxdepth = 0;
	parser->atmospheric = 0;
	parser->avgdepth = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
		parser->gasmix[i].type = 0;
		parser->gasmix[i].id = 0;
	}
	parser->diluent = UNDEFINED;
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].volume = 0;
		parser->tank[i].workpressure = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tank[i].transmitter = 0;
		parser->tank[i].active = 0;
	}
	parser->vpm = 0;
	parser->gf_lo = 0;
	parser->gf_hi = 0;
	parser->seawater = 0;
	for (unsigned int i = 0; i < NSENSORS; ++i) {
		parser->calibration[i] = 0;
	}
	parser->calibrated = 0;
	parser->have_location = 0;
	parser->latitude = 0;
	parser->longitude = 0;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_freedom_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesoft_freedom_parser_t *parser = (divesoft_freedom_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the header data.
	status = divesoft_freedom_cache (parser);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned int timestamp = array_uint32_le (data + 8);

	int timezone = 0;
	if (parser->version == HEADER_SIGNATURE_V2) {
		timezone = ((signed short) array_uint16_le (data + 40)) * 60;
	} else {
		timezone = 0;
	}

	dc_ticks_t ticks = (dc_ticks_t) timestamp + EPOCH + timezone;

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	if (parser->version == HEADER_SIGNATURE_V2) {
		datetime->timezone = timezone;
	} else {
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_freedom_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesoft_freedom_parser_t *parser = (divesoft_freedom_parser_t *) abstract;

	// Cache the header data.
	status = divesoft_freedom_cache (parser);
	if (status != DC_STATUS_SUCCESS)
		return status;

	dc_salinity_t *water = (dc_salinity_t *) value;
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
		case DC_FIELD_AVGDEPTH:
			if (parser->version != HEADER_SIGNATURE_V2)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = parser->avgdepth / 100.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = parser->temperature_min / 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = parser->atmospheric * 10.0 / BAR;
			break;
		case DC_FIELD_SALINITY:
			water->type = parser->seawater ? DC_WATER_SALT : DC_WATER_FRESH;
			water->density = parser->seawater ? SEAWATER : FRESHWATER;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->divemode) {
			case STMODE_OC:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case STMODE_CCR:
			case STMODE_MCCR:
			case STMODE_BOCCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
				break;
			case STMODE_FREE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			case STMODE_GAUGE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case STMODE_ASCR:
			case STMODE_PSCR:
				*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
				break;
			case STMODE_UNKNOWN:
				return DC_STATUS_UNSUPPORTED;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			if (parser->gasmix[flags].type == OXYGEN) {
				gasmix->usage = DC_USAGE_OXYGEN;
			} else if (parser->gasmix[flags].type == DILUENT) {
				gasmix->usage = DC_USAGE_DILUENT;
			} else {
				gasmix->usage = DC_USAGE_NONE;
			}
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			if (parser->tank[flags].volume > 990 ||
				parser->tank[flags].workpressure > 400) {
				tank->type = DC_TANKVOLUME_NONE;
				tank->volume = 0.0;
				tank->workpressure = 0.0;
			} else {
				tank->type = DC_TANKVOLUME_METRIC;
				tank->volume = parser->tank[flags].volume / 10.0;
				tank->workpressure  = parser->tank[flags].workpressure;
			}
			tank->beginpressure = parser->tank[flags].beginpressure * 2.0;
			tank->endpressure   = parser->tank[flags].endpressure   * 2.0;
			tank->gasmix = flags;
			tank->usage = DC_USAGE_NONE;
			break;
		case DC_FIELD_DECOMODEL:
			if (parser->vpm) {
				decomodel->type = DC_DECOMODEL_VPM;
				decomodel->conservatism = 0;
			} else {
				decomodel->type = DC_DECOMODEL_BUHLMANN;
				decomodel->conservatism = 0;
				decomodel->params.gf.low = parser->gf_lo;
				decomodel->params.gf.high = parser->gf_hi;
			}
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
divesoft_freedom_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesoft_freedom_parser_t *parser = (divesoft_freedom_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the header data.
	status = divesoft_freedom_cache (parser);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned int time = UNDEFINED;
	unsigned int initial = 0;
	unsigned int offset = parser->headersize;
	while (offset + RECORD_SIZE <= size) {
		dc_sample_value_t sample = {0};

		if (array_isequal(data + offset, RECORD_SIZE, 0xFF)) {
			WARNING (abstract->context, "Skipping empty sample.");
			offset += RECORD_SIZE;
			continue;
		}

		unsigned int flags = array_uint32_le (data + offset);
		unsigned int type      = (flags & 0x0000000F) >> 0;
		unsigned int timestamp = (flags & 0x001FFFF0) >> 4;
		unsigned int id        = (flags & 0x7FE00000) >> 21;

		if (timestamp != time) {
			if (timestamp < time && time != UNDEFINED) {
				// The timestamp are supposed to be monotonically increasing,
				// but occasionally there are small jumps back in time with just
				// 1 or 2 seconds. To get back in sync, those samples are
				// skipped. Larger jumps are treated as errors.
				if (time - timestamp > 5) {
					ERROR (abstract->context, "Timestamp moved backwards (%u %u).", timestamp, time);
					return DC_STATUS_DATAFORMAT;
				}
				WARNING (abstract->context, "Timestamp moved backwards (%u %u).", timestamp, time);
				offset += RECORD_SIZE;
				continue;
			}
			time = timestamp;
			sample.time = time * 1000;
			if (callback) callback(DC_SAMPLE_TIME, &sample, userdata);
		}

		// Initial diluent.
		if (!initial) {
			if (parser->diluent != UNDEFINED) {
				sample.gasmix = parser->diluent;
				if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
			}
			initial = 1;
		}

		if (type == LREC_POINT) {
			// General log record.
			unsigned int depth = array_uint16_le (data + offset + 4);
			unsigned int ppo2  = array_uint16_le (data + offset + 6);

			sample.depth = depth / 100.0;
			if (callback) callback(DC_SAMPLE_DEPTH, &sample, userdata);

			if (ppo2) {
				sample.ppo2.sensor = DC_SENSOR_NONE;
				sample.ppo2.value = ppo2 * 10.0 / BAR;
				if (callback) callback(DC_SAMPLE_PPO2, &sample, userdata);
			}

			if (id == POINT_2) {
				unsigned int orientation = array_uint32_le (data + offset + 8);
				unsigned int heading = orientation & 0x1FF;
				sample.bearing = heading;
				if (callback) callback (DC_SAMPLE_BEARING, &sample, userdata);
			} else if (id == POINT_1 || id == POINT_1_OLD) {
				unsigned int misc = array_uint32_le (data + offset + 8);
				unsigned int ceiling = array_uint16_le (data + offset + 12);
				unsigned int setpoint = data[offset + 15];
				unsigned int ndl  = (misc & 0x000003FF);
				unsigned int tts  = (misc & 0x000FFC00) >> 10;
				unsigned int temp = (misc & 0x3FF00000) >> 20;

				// Temperature
				sample.temperature = (signed int) signextend (temp, 10) / 10.0;
				if (callback) callback(DC_SAMPLE_TEMPERATURE, &sample, userdata);

				// Deco / NDL
				if (ceiling) {
					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.time = 0;
					sample.deco.depth = ceiling / 100.0;
				} else {
					sample.deco.type = DC_DECO_NDL;
					sample.deco.time = ndl * 60;
					sample.deco.depth = 0.0;
				}
				sample.deco.tts = tts * 60;
				if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);

				// Setpoint
				if (setpoint) {
					sample.setpoint = setpoint / 100.0;
					if (callback) callback(DC_SAMPLE_SETPOINT, &sample, userdata);
				}
			}
		} else if ((type >= LREC_MANIPULATION && type <= LREC_ACTIVITY) || type == LREC_INFO) {
			// Event record.
			unsigned int event = array_uint16_le (data + offset + 4);

			if (event == EVENT_BOOKMARK) {
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback(DC_SAMPLE_EVENT, &sample, userdata);
			} else if (event == EVENT_MIX_CHANGED || event == EVENT_DILUENT || event == EVENT_CHANGE_MODE) {
				unsigned int o2 = data[offset + 6];
				unsigned int he = data[offset + 7];
				unsigned int mixtype = OC;
				if (event == EVENT_DILUENT) {
					mixtype = DILUENT;
				} else if (event == EVENT_CHANGE_MODE) {
					unsigned int mode = data[offset + 8];
					if (divesoft_freedom_is_ccr (mode)) {
						mixtype = DILUENT;
					}
				}

				unsigned int idx = divesoft_freedom_find_gasmix (parser->gasmix, parser->ngasmixes, o2, he, mixtype);
				if (idx >= parser->ngasmixes) {
					ERROR (abstract->context, "Gas mix (%u/%u) not found.", o2, he);
					return DC_STATUS_DATAFORMAT;
				}
				sample.gasmix = idx;
				if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
			} else if (event == EVENT_CNS) {
				sample.cns = array_uint16_le (data + offset + 6) / 100.0;
				if (callback) callback(DC_SAMPLE_CNS, &sample, userdata);
			} else if (event == EVENT_SETPOINT_MANUAL || event == EVENT_SETPOINT_AUTO) {
				sample.setpoint = data[6] / 100.0;
				if (callback) callback(DC_SAMPLE_SETPOINT, &sample, userdata);
			}
		} else if (type == LREC_MEASURE) {
			// Measurement record.
			if (id == MEASURE_ID_AI_PRESSURE) {
				for (unsigned int i = 0; i < NTANKS; ++i) {
					unsigned int pressure = data[offset + 4 + i];
					if (pressure == 0 || pressure == 0xFF)
						continue;

					unsigned int idx = divesoft_freedom_find_tank (parser->tank, parser->ntanks, i);
					if (idx >= parser->ntanks) {
						ERROR (abstract->context, "Tank %u not found.", idx);
						return DC_STATUS_DATAFORMAT;
					}

					sample.pressure.tank = idx;
					sample.pressure.value = pressure * 2.0;
					if (callback) callback(DC_SAMPLE_PRESSURE, &sample, userdata);
				}
			} else if (id == MEASURE_ID_OXYGEN) {
				for (unsigned int i = 0; i < NSENSORS; ++i) {
					unsigned int ppo2 = array_uint16_le (data + offset + 4 + i * 2);
					if (ppo2 == 0 || ppo2 == 0xFFFF)
						continue;
					sample.ppo2.sensor = i;
					sample.ppo2.value = ppo2 * 10.0 / BAR;
					if (callback) callback(DC_SAMPLE_PPO2, &sample, userdata);
				}
			} else if (id == MEASURE_ID_OXYGEN_MV) {
				for (unsigned int i = 0; i < NSENSORS; ++i) {
					unsigned int value = array_uint16_le (data + offset + 4 + i * 2);
					unsigned int state = data[offset + 12 + i];
					if (!parser->calibrated || parser->calibration[i] == 0 ||
						state == SENSTAT_UNCALIBRATED || state == SENSTAT_NOT_EXIST)
						continue;
					sample.ppo2.sensor = i;
					sample.ppo2.value = value / 100.0 * parser->calibration[i] / BAR;
					if (callback) callback(DC_SAMPLE_PPO2, &sample, userdata);
				}
			}
		} else if (type == LREC_STATE) {
			// Tissue saturation record.
		}

		offset += RECORD_SIZE;
	}

	return DC_STATUS_SUCCESS;
}
