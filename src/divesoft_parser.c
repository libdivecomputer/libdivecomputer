/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Ryan Gardner, Jef Driesen
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include <libdivecomputer/units.h>

#include "divesoft.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define HEADER_SIGNATURE_V1 0x45766944 // "DivE"
#define HEADER_SIGNATURE_V2 0x45566944 // "DiVE"

#define HEADER_V1_SIZE 32
#define HEADER_V2_SIZE 64

typedef struct divesoft_field_offset_t {
    unsigned offset; // byte offset of the field
    unsigned shift;  // bit offset in the 32bit area (little endian)
    unsigned length; // bit length
} divesoft_field_offset_t;

typedef struct divesoft_dive_header_offsets_t {
    unsigned header_size;
    divesoft_field_offset_t datum;
    divesoft_field_offset_t records;
    divesoft_field_offset_t mode;
    divesoft_field_offset_t duration;
    divesoft_field_offset_t max_depth;
    divesoft_field_offset_t min_temp;
    divesoft_field_offset_t p_air;
} divesoft_dive_header_info_t;

static const divesoft_dive_header_info_t divesoft_dive_header_v1_offsets = {
        HEADER_V1_SIZE,
        {8, 0, 32},
        {16, 0, 18},
        {12, 27, 3},
        {12, 0, 17},
        {20, 0, 16},
        {16, 18, 10},
        {24, 0, 16},
};
static const divesoft_dive_header_info_t divesoft_dive_header_v2_offsets = {
        HEADER_V2_SIZE,
        {8, 0, 32},
        {20, 0, 32},
        {18, 0, 8},
        // .
        {12, 0, 32},
        {28, 0, 16},
        {24, 0, 16},
        {32, 0, 16},
};

static unsigned divesoft_read_field(const unsigned char * data, divesoft_field_offset_t field) {
    unsigned int mask = 0xFFFFFFFF >> (32 - field.length);
    return (array_uint32_le(data + field.offset) >> field.shift) & mask;
}

#define     RO_SEAWATER             (1028)          // [kg m-3]
#define     RO_FRESHWATER           (1000)          // [kg m-3]

#define LOG_PRODUCT_CODE_LENGTH 4
#define LOG_SERIAL_NUMBER_LENGTH 8

// dive record type
typedef enum {
    LREC_POINT,
    // events
    LREC_MANIPULATION,
    LREC_AUTO,
    LREC_DIVER_ERROR,
    LREC_INTERNAL_ERROR,
    LREC_ACTIVITY,
    // not events, have own structure
    LREC_CONFIGURATION,
    LREC_MEASURE,
    LREC_STATE,
    LREC_INFO,
    LREC_LAST = LREC_INFO
} tLogRecType;

// event types
typedef enum {
    EVENT_DUMMY,
    EVENT_SETPOINT_MANUAL,
    EVENT_SETPOINT_AUTO,
    _EVENT_OC,
    _EVENT_CCR,
    EVENT_MIX_CHANGED,
    EVENT_START,
    EVENT_TOO_FAST,
    EVENT_ABOVE_CEILING,
    EVENT_TOXIC,
    EVENT_HYPOX,
    EVENT_CRITICAL,
    EVENT_SENSOR_DISABLED,
    EVENT_SENSOR_ENABLED,
    EVENT_O2_BACKUP,
    EVENT_PEER_DOWN,
    EVENT_HS_DOWN,
    EVENT_INCONSISTENT,
    EVENT_KEYDOWN,
    _EVENT_SCR,
    EVENT_ABOVE_STOP,
    EVENT_SAFETY_MISS,
    EVENT_FATAL,
    EVENT_DILUENT,
    EVENT_CHANGE_MODE,
    EVENT_SOLENOID,
    EVENT_BOOKMARK,
    EVENT_GF_SWITCH,
    EVENT_PEER_UP,
    EVENT_HS_UP,
    EVENT_CNS,
    EVENT_BATTERY_LOW,
    EVENT_PPO2_LOST,
    EVENT_SENSOR_VALUE_BAD,
    EVENT_SAFETY_STOP_END,
    EVENT_DECO_STOP_END,
    EVENT_DEEP_STOP_END,
    EVENT_NODECO_END,
    EVENT_DEPTH_REACHED,
    EVENT_TIME_ELAPSED,
    EVENT_STACK_USAGE,
    EVENT_GAS_SWITCH_INFO,
    EVENT_PRESSURE_SENS_WARN,
    EVENT_PRESSURE_SENS_FAIL,
    EVENT_CHECK_O2_SENSORS,
    EVENT_SWITCH_TO_COMP_SCR,
    EVENT_GAS_LOST,
    EVENT_AIRBREAK,
    EVENT_AIRBREAK_END,
    EVENT_AIRBREAK_MISSED,
    EVENT_BORMT_EXPIRATION,
    EVENT_BORMT_EXPIRED,
    EVENT_SENSOR_EXCLUDED,
    EVENT_PREBR_SKIPPED,
    EVENT_BOCCR_BORMT_EXPIRED,
    EVENT_WAYPOINT,
    EVENT_TURNAROUND,
    EVENT_SOLENOID_FAILURE,
    EVENT_SM_CYL_PRESS_DIFF,
    EVENT_LAST = EVENT_SM_CYL_PRESS_DIFF
} tDiveEventType;

// config record id
typedef enum {
    CFG_TEST_CCR_FULL_1         = 0,
    CFG_TEST_CCR_PARTIAL_1      = 1,
    CFG_OXYGEN_CALIBRATION      = 2,
    CFG_SERIAL                  = 3,
    CFG_CONFIG_DECO             = 4,
    CFG_VERSION                 = 5,
    CFG_CONFIG_ASCENT           = 6,
    CFG_CONFIG_AI               = 7,
    CFG_CONFIG_CCR              = 8,
    CFG_CONFIG_DILUENTS         = 9,
    CFG_LAST = CFG_CONFIG_DILUENTS
} t_configuration_id;

// measurement record id
typedef enum {
    MEASURE_ID_OXYGEN       =  0,
    MEASURE_ID_BATTERY      =  1,
    MEASURE_ID_HELIUM       =  2,
    MEASURE_ID_OXYGEN_MV    =  3,
    MEASURE_ID_GPS          =  4,
    MEASURE_ID_PRESSURE     =  5,
    MEASURE_ID_AI_SAC       =  6,
    MEASURE_ID_AI_PRESSURE  =  7,
    MEASURE_ID_BRIGHTNESS   =  8,
    MEASURE_ID_AI_STAT      =  9,
    MEASURE_ID_LAST = MEASURE_ID_AI_STAT
} t_measure_id;

// state record id
typedef enum {
    STATE_ID_DECO_N2LOW,
    STATE_ID_DECO_N2HIGH,
    STATE_ID_DECO_HELOW,
    STATE_ID_DECO_HEHIGH,
    STATE_ID_PLAN_STEPS,
    STATE_ID_LAST = STATE_ID_PLAN_STEPS
} t_state_id;

// setpoint change reason
typedef enum {
    SP_MANUAL,
    SP_AUTO_START,
    SP_AUTO_HYPOX,
    SP_AUTO_TIMEOUT,
    SP_AUTO_ASCENT,
    SP_AUTO_STALL,
    SP_AUTO_SPLOW,
    SP_AUTO_DEPTH_DESC,
    SP_AUTO_DEPTH_ASC,
    SP_LAST = SP_AUTO_DEPTH_ASC
} tSetpointChangeReason;

// dive start method
typedef enum {
    START_MANUAL,               ///< manually by choosing Start dive
    START_NORMAL,               ///< manually by diving 1.5m deep with computer turned on
    START_EMERGENCY,            ///< with computer turned off
    START_LAST = START_EMERGENCY
} tDiveStartReason;

// computer mode when dive started
typedef enum {
    STMODE_UNKNOWN,
    STMODE_OC,
    STMODE_CCR,
    STMODE_mCCR,
    STMODE_FREE,
    STMODE_GAUGE,
    STMODE_ASCR,
    STMODE_PSCR,
    STMODE_BOCCR,
    STMODE_LAST = STMODE_BOCCR
} tDiveStartMode;

// water salinity
typedef enum {
    SALINITY_FRESH,
    SALINITY_SEA,
} t_log_salinity;

//BoCCR bailout remaining time limiting factor
typedef enum {
    BOCCR_BORMT_STACK,
    BOCCR_BORMT_BATTERY,
    BOCCR_BORMT_CNS,
    BOCCR_BORMT_OXY_PRESSURE,
    BOCCR_BORMT_DIL_PRESSURE,
    BOCCR_BORMT_LAST = BOCCR_BORMT_DIL_PRESSURE,
} t_boccr_bormt_reason;

// sensor status
typedef enum {
    SENSTAT_NORMAL          =  0,
    SENSTAT_OVERRANGE       =  1,
    SENSTAT_DISABLED        =  2,
    SENSTAT_EXCLUDED        =  3,
    SENSTAT_UNCALIBRATED    =  4,
    SENSTAT_ERROR           =  5,
    SENSTAT_OFFLINE         =  6,
    SENSTAT_INHIBITED       =  7,
    SENSTAT_NOT_EXIST       =  8,
    SENSTAT_LAST = SENSTAT_NOT_EXIST
} t_log_sensor_status;

// battery status
typedef enum {
    BATSTATE_NO_BATTERY,
    BATSTATE_UNKNOWN,
    BATSTATE_DISCHARGING,
    BATSTATE_CHARGING,
    BATSTATE_FULL,
    BATSTATE_LAST = BATSTATE_FULL
} t_log_battery_state;

// plan step type
typedef enum {
    LPST_UNDEFINED,
    LPST_DECOSTOP,
    LPST_DECOSTOP_GAS,
    LPST_GAS_SWITCH,
    LPST_DEEPSTOP,
    LPST_SAFETYSTOP,
    LPST_ERROR = 15,
    LPST_LAST = LPST_ERROR
} t_log_plan_step_type;

// LREC_POINT types
typedef enum {
    POINT_1 = 0,               // record with extended deco data
    POINT_2 = 1,               // record with extended navigational data
    POINT_1_OLD = 0x3FF        // legacy compatible, 10 bit record
} t_point_id;

#define    LOG_PRESSURE_TEMP_OFFSET       (30)      // 30st

// size of dive record
#define DIVEREC_SIZE 16

// dive record field locations
static const divesoft_field_offset_t diverec_type = { 0, 0, 4 }; // record type, tLogRecType
static const divesoft_field_offset_t diverec_time = { 0, 4, 17 }; // time from dive beginning [s] (max 36h)
static const divesoft_field_offset_t diverec_id = { 0, 21, 10 }; // record id in scope of its type
static const divesoft_field_offset_t diverec_novr = { 0, 31, 1 }; // 1 if record is ok; (otherwise some overflow happened)

static const divesoft_field_offset_t point_depth = { 4, 0, 16 }; // depth in cm
static const divesoft_field_offset_t point_ppo2 = { 6, 0, 16 };  // oxygen partial pressure in 10Pa
static const divesoft_field_offset_t point_heading = { 8, 0, 9 }; // POINT_2 only, heading in 0-359 degree range
static const divesoft_field_offset_t point_temperature = { 8, 20, 10 }; // POINT_1 only, temperature in 0.1 deg. C

static const divesoft_field_offset_t measure_temperature = { 8, 0, 8 }; // CCR loop temperature, in 0.5C, signed, offset 30C
static const divesoft_field_offset_t measure_ai_pressure = { 4, 0, 8 }; // cylinder pressure, in 2bar resolution

static const divesoft_field_offset_t event_type = { 4, 0, 16 }; // event type
static const divesoft_field_offset_t event_cns = { 6, 0, 16 }; // cns, in %
static const divesoft_field_offset_t event_rate = { 6, 0, 16 }; // rate, in cm/min
static const divesoft_field_offset_t event_mix_o2 = { 6, 0, 8 }; // % of mixture
static const divesoft_field_offset_t event_mix_he = { 7, 0, 8 }; // % of mixture

static const divesoft_field_offset_t config_deco_seawater = { 4, 1, 1 }; // seawater? 1 = yes
static const divesoft_field_offset_t config_deco_vpm = { 4, 5, 1 }; // vpm? 1 = yes
static const divesoft_field_offset_t config_deco_gf_lo = { 8, 0, 8 }; // gf low
static const divesoft_field_offset_t config_deco_gf_hi = { 9, 0, 8 }; // gf high

#define MAX_GASMIXES 10

typedef struct divesoft_parser_t {
	dc_parser_t base;
    int ngasmixes;
    unsigned o2[MAX_GASMIXES];
    unsigned he[MAX_GASMIXES];
} divesoft_parser_t;

static dc_status_t divesoft_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t divesoft_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t divesoft_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t divesoft_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t divesoft_parser_vtable = {
	sizeof(divesoft_parser_t),
	DC_FAMILY_DIVESOFT,
	divesoft_parser_set_data, /* set_data */
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	divesoft_parser_get_datetime, /* datetime */
	divesoft_parser_get_field, /* fields */
	divesoft_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
divesoft_parser_create (dc_parser_t **out, dc_context_t *context)
{
	divesoft_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (divesoft_parser_t *) dc_parser_allocate (context, &divesoft_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

    parser->ngasmixes = 0;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}

static const divesoft_dive_header_info_t *
divesoft_header_check(const unsigned char * data, unsigned size)
{
    if (size < 4)
        return NULL;

    // header version detection
    unsigned version_signature = array_uint32_le(data);
    const divesoft_dive_header_info_t * header_info;
    if(version_signature == HEADER_SIGNATURE_V1) {
        header_info = &divesoft_dive_header_v1_offsets;
    }
    else if(version_signature == HEADER_SIGNATURE_V2) {
        header_info = &divesoft_dive_header_v2_offsets;
    }
    else {
        return NULL;
    }

    if (size < header_info->header_size) {
        return NULL;
    }

    return header_info;
}

#define TIMESTAMP_BASE 946684800 // 1st Jan 2000 00:00:00

static dc_status_t
divesoft_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

    const divesoft_dive_header_info_t * header_info = divesoft_header_check(data, size);
    if(!header_info) {
        return DC_STATUS_DATAFORMAT;
    }

    dc_datetime_t date;
    dc_datetime_gmtime(&date, TIMESTAMP_BASE + divesoft_read_field(data, header_info->datum));

	if (datetime) {
		datetime->year = date.year;
		datetime->month = date.month;
		datetime->day = date.day;
		datetime->hour = date.hour;
		datetime->minute = date.minute;
		datetime->second = date.second;
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{    
    divesoft_parser_t *parser = (divesoft_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

    const divesoft_dive_header_info_t * header_info = divesoft_header_check(data, size);
    if(!header_info) {
        return DC_STATUS_DATAFORMAT;
    }

    const unsigned char *data_rec = data + header_info->header_size;
    unsigned int size_rec = size - header_info->header_size;

    // we have to find configuration rec for salinity
    unsigned seawater = 0;
    unsigned vpm = 0;
    unsigned gf_lo = 0;
    unsigned gf_hi = 0;
    for(size_t i = 0; i < size_rec; i += DIVEREC_SIZE) {
        const unsigned char *r = &data_rec[i];
        unsigned rec_type = divesoft_read_field(r, diverec_type);
        unsigned id = divesoft_read_field(r, diverec_id);
        if(rec_type == LREC_CONFIGURATION && id == CFG_CONFIG_DECO) {
            seawater = divesoft_read_field(r, config_deco_seawater);
            vpm = divesoft_read_field(r, config_deco_vpm);
            gf_lo = divesoft_read_field(r, config_deco_gf_lo);
            gf_hi = divesoft_read_field(r, config_deco_gf_hi);
        }
        if(rec_type == LREC_MANIPULATION) {
            unsigned ev_type = divesoft_read_field(r, event_type);
            if(ev_type == EVENT_MIX_CHANGED) {
                unsigned o2 = divesoft_read_field(r, event_mix_o2);
                unsigned he = divesoft_read_field(r, event_mix_he);
                if(o2 == 0xFF || he == 0xFF) continue; // not valid data, measuring mixes...
                // lookup gas mix in table
                int result = parser->ngasmixes;
                for(int g = 0; g < parser->ngasmixes; g++) {
                    if(parser->o2[g] == o2 && parser->he[g] == he) {
                        result = g;
                        break;
                    }
                }
                if(result >= MAX_GASMIXES) {
                    // there is not enough room for another gas mix
                    continue; // fail silently... might be DATAFORMAT error
                }
                if(result == parser->ngasmixes) {
                    parser->ngasmixes++;
                    parser->o2[result] = o2;
                    parser->he[result] = he;
                }
            }
        }
    }

	dc_salinity_t *water = (dc_salinity_t *) value;
    dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
    dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = divesoft_read_field(data, header_info->duration);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = ((unsigned short) divesoft_read_field(data, header_info->max_depth)) / 100.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = ((short) divesoft_read_field(data, header_info->min_temp)) / 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = ((unsigned short) divesoft_read_field(data, header_info->p_air)) / 10000.0;
			break;
		case DC_FIELD_SALINITY:
			water->type = seawater ? DC_WATER_SALT : DC_WATER_FRESH;
			water->density = seawater ? RO_SEAWATER : RO_FRESHWATER;
			break;
		case DC_FIELD_DIVEMODE:
			switch (divesoft_read_field(data, header_info->mode)) {
                case STMODE_UNKNOWN: // unknown
                case STMODE_OC:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_OC;
                    break;
                case STMODE_CCR:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
                    break;
                case STMODE_mCCR:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
                    break;
                case STMODE_FREE:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
                    break;
                case STMODE_GAUGE:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_OC;
                    break;
                case STMODE_ASCR:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
                    break;
                case STMODE_PSCR:
                    *((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
                    break;
            }
			break;
        case DC_FIELD_GASMIX_COUNT:
            *((unsigned int *) value) = parser->ngasmixes;
            break;
        case DC_FIELD_GASMIX:
            gasmix->helium = parser->he[flags] / 100.0;
            gasmix->oxygen = parser->o2[flags] / 100.0;
            gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
            break;
        case DC_FIELD_DECOMODEL:
            if(vpm) {
                decomodel->type = DC_DECOMODEL_VPM;
                decomodel->conservatism = 0;
            }
            else {
                decomodel->type = DC_DECOMODEL_BUHLMANN;
                decomodel->conservatism = 0;
                decomodel->params.gf.low = gf_lo;
                decomodel->params.gf.high = gf_hi;
            }
            break;
		default:
            //return DC_STATUS_SUCCESS;
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
    dc_status_t status = DC_STATUS_SUCCESS;
    divesoft_parser_t *parser = (divesoft_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

    const divesoft_dive_header_info_t * header_info = divesoft_header_check(data, size);
    if(!header_info) {
        return DC_STATUS_DATAFORMAT;
    }

    data += header_info->header_size;
    size -= header_info->header_size;

    if (size % DIVEREC_SIZE != 0) {
        ERROR (abstract->context, "Not a multiple of diverec! Size is %u\n", size);
        return DC_STATUS_DATAFORMAT;
    }

    if(!callback) return status; // no parsing needed, nobody would receive the data

    dc_sample_value_t sample = { 0 };
    for(size_t i = 0; i < size; i += DIVEREC_SIZE) {
        const unsigned char * r = &data[i];
        sample.time = divesoft_read_field(r, diverec_time);
        callback(DC_SAMPLE_TIME, sample, userdata);
        unsigned type = divesoft_read_field(r, diverec_type);
        unsigned id = divesoft_read_field(r, diverec_id);
        if(type == LREC_POINT) {
            sample.depth = divesoft_read_field(r, point_depth) / 100.0;
            callback(DC_SAMPLE_DEPTH, sample, userdata);
            sample.ppo2 = divesoft_read_field(r, point_ppo2) / 10000.0;
            callback(DC_SAMPLE_PPO2, sample, userdata);
            if(id == POINT_2) {
                sample.bearing = divesoft_read_field(r, point_heading);
                callback(DC_SAMPLE_BEARING, sample, userdata);
            }
            else if(id == POINT_1) {
                int temperature = divesoft_read_field(r, point_temperature);
                if(temperature >= 512) temperature -= 1024;
                sample.temperature = temperature / 0.1;
                callback(DC_SAMPLE_TEMPERATURE, sample, userdata);
            }
        }
        if(type == LREC_MEASURE) {
            if(id == MEASURE_ID_PRESSURE) {
                sample.temperature = ((char) divesoft_read_field(r, measure_temperature)) * 0.5 + LOG_PRESSURE_TEMP_OFFSET;
                callback(DC_SAMPLE_TEMPERATURE, sample, userdata);
            }
            if(id == MEASURE_ID_AI_PRESSURE) {
                sample.pressure.tank = 0;
                sample.pressure.value = divesoft_read_field(r, measure_ai_pressure) * 2.0;
                callback(DC_SAMPLE_PRESSURE, sample, userdata);
            }
        }
        if(type == LREC_DIVER_ERROR) {
            unsigned ev_type = divesoft_read_field(r, event_type);
            sample.event.time = 0;
            sample.event.flags = 0;
            sample.event.value = 0;
            if(ev_type == EVENT_CNS) {
                sample.cns = divesoft_read_field(r, event_cns);
                callback(DC_SAMPLE_CNS, sample, userdata);
            }
            else if(ev_type == EVENT_ABOVE_CEILING) {
                sample.event.type = SAMPLE_EVENT_CEILING;
                callback(DC_SAMPLE_EVENT, sample, userdata);
            }
            else if(ev_type == EVENT_TOO_FAST) {
                sample.event.type = SAMPLE_EVENT_ASCENT;
                sample.event.value = divesoft_read_field(r, event_rate); // todo: unit conversion?
                callback(DC_SAMPLE_EVENT, sample, userdata);
            }
            else if(ev_type == EVENT_ABOVE_STOP) {
                sample.event.type = SAMPLE_EVENT_DECOSTOP;
                callback(DC_SAMPLE_EVENT, sample, userdata);
            }
            else if(ev_type == EVENT_SAFETY_MISS) {
                sample.event.type = SAMPLE_EVENT_SAFETYSTOP;
                callback(DC_SAMPLE_EVENT, sample, userdata);
            }
        }
        if(type == LREC_MANIPULATION) {
            unsigned ev_type = divesoft_read_field(r, event_type);
            if(ev_type == EVENT_BOOKMARK) {
                sample.event.type = SAMPLE_EVENT_BOOKMARK;
                callback(DC_SAMPLE_EVENT, sample, userdata);
            }
            else if(ev_type == EVENT_MIX_CHANGED) {
                unsigned o2 = divesoft_read_field(r, event_mix_o2);
                unsigned he = divesoft_read_field(r, event_mix_he);
                if(o2 == 0xFF || he == 0xFF) continue; // not valid data, measuring mixes...
                // lookup gas mix in table
                int result = parser->ngasmixes;
                for(int g = 0; g < parser->ngasmixes; g++) {
                    if(parser->o2[g] == o2 && parser->he[g] == he) {
                        result = g;
                        break;
                    }
                }
                if(result == parser->ngasmixes) {
                    // gas mix not in table
                    continue; // fail silently... might be DATAFORMAT error
                }
                sample.gasmix = result;
                callback(DC_SAMPLE_GASMIX, sample, userdata);
            }
        }
        if(type == LREC_INFO) {
            unsigned ev_type = divesoft_read_field(r, event_type);
            if(ev_type == EVENT_DECO_STOP_END) {
                sample.deco.type = DC_DECO_DECOSTOP;
            }
            else if(ev_type == EVENT_SAFETY_STOP_END) {
                sample.deco.type = DC_DECO_SAFETYSTOP;
            }
            else if(ev_type == EVENT_DEEP_STOP_END) {
                sample.deco.type = DC_DECO_DEEPSTOP;
            }
            else if(ev_type == EVENT_NODECO_END) {
                sample.deco.type = DC_DECO_NDL;
            }
            sample.deco.time = 0;
            sample.deco.depth = sample.depth;
            callback(DC_SAMPLE_DECO, sample, userdata);
        }
        if(type == LREC_LAST) {
            // final diverec, we can end
            break;
        }
    }

	return DC_STATUS_SUCCESS;
}
