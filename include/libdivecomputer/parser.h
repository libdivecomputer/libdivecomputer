/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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

#ifndef DC_PARSER_H
#define DC_PARSER_H

#include "common.h"
#include "context.h"
#include "descriptor.h"
#include "device.h"
#include "datetime.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dc_sample_type_t {
	DC_SAMPLE_TIME,
	DC_SAMPLE_DEPTH,
	DC_SAMPLE_PRESSURE,
	DC_SAMPLE_TEMPERATURE,
	DC_SAMPLE_EVENT,
	DC_SAMPLE_RBT,
	DC_SAMPLE_HEARTBEAT,
	DC_SAMPLE_BEARING,
	DC_SAMPLE_VENDOR,
	DC_SAMPLE_SETPOINT,
	DC_SAMPLE_PPO2,
	DC_SAMPLE_CNS,
	DC_SAMPLE_DECO,
	DC_SAMPLE_GASMIX
} dc_sample_type_t;

typedef enum dc_field_type_t {
	DC_FIELD_DIVETIME,
	DC_FIELD_MAXDEPTH,
	DC_FIELD_AVGDEPTH,
	DC_FIELD_GASMIX_COUNT,
	DC_FIELD_GASMIX,
	DC_FIELD_SALINITY,
	DC_FIELD_ATMOSPHERIC,
	DC_FIELD_TEMPERATURE_SURFACE,
	DC_FIELD_TEMPERATURE_MINIMUM,
	DC_FIELD_TEMPERATURE_MAXIMUM,
	DC_FIELD_TANK_COUNT,
	DC_FIELD_TANK,
	DC_FIELD_DIVEMODE,
	DC_FIELD_DECOMODEL,
	DC_FIELD_LOCATION,
} dc_field_type_t;

typedef enum parser_sample_event_t {
	SAMPLE_EVENT_NONE,
	SAMPLE_EVENT_DECOSTOP,
	SAMPLE_EVENT_RBT,
	SAMPLE_EVENT_ASCENT,
	SAMPLE_EVENT_CEILING,
	SAMPLE_EVENT_WORKLOAD,
	SAMPLE_EVENT_TRANSMITTER,
	SAMPLE_EVENT_VIOLATION,
	SAMPLE_EVENT_BOOKMARK,
	SAMPLE_EVENT_SURFACE,
	SAMPLE_EVENT_SAFETYSTOP,
	SAMPLE_EVENT_GASCHANGE, /* Deprecated: replaced by DC_SAMPLE_GASMIX. */
	SAMPLE_EVENT_SAFETYSTOP_VOLUNTARY,
	SAMPLE_EVENT_SAFETYSTOP_MANDATORY,
	SAMPLE_EVENT_DEEPSTOP,
	SAMPLE_EVENT_CEILING_SAFETYSTOP,
	SAMPLE_EVENT_FLOOR,
	SAMPLE_EVENT_DIVETIME,
	SAMPLE_EVENT_MAXDEPTH,
	SAMPLE_EVENT_OLF,
	SAMPLE_EVENT_PO2,
	SAMPLE_EVENT_AIRTIME,
	SAMPLE_EVENT_RGBM,
	SAMPLE_EVENT_HEADING,
	SAMPLE_EVENT_TISSUELEVEL,
	SAMPLE_EVENT_GASCHANGE2, /* Deprecated: replaced by DC_SAMPLE_GASMIX. */
} parser_sample_event_t;

/* For backwards compatibility */
#define SAMPLE_EVENT_UNKNOWN SAMPLE_EVENT_FLOOR

typedef enum parser_sample_flags_t {
	SAMPLE_FLAGS_NONE = 0,
	SAMPLE_FLAGS_BEGIN = (1 << 0),
	SAMPLE_FLAGS_END = (1 << 1)
} parser_sample_flags_t;

typedef enum parser_sample_vendor_t {
	SAMPLE_VENDOR_NONE,
	SAMPLE_VENDOR_UWATEC_ALADIN,
	SAMPLE_VENDOR_UWATEC_SMART,
	SAMPLE_VENDOR_OCEANIC_VTPRO,
	SAMPLE_VENDOR_OCEANIC_VEO250,
	SAMPLE_VENDOR_OCEANIC_ATOM2
} parser_sample_vendor_t;

typedef enum dc_water_t {
	DC_WATER_FRESH,
	DC_WATER_SALT
} dc_water_t;

typedef enum dc_divemode_t {
	DC_DIVEMODE_FREEDIVE,
	DC_DIVEMODE_GAUGE,
	DC_DIVEMODE_OC, /* Open circuit */
	DC_DIVEMODE_CCR, /* Closed circuit rebreather */
	DC_DIVEMODE_SCR  /* Semi-closed circuit rebreather */
} dc_divemode_t;

/* For backwards compatibility */
#define DC_DIVEMODE_CC DC_DIVEMODE_CCR

typedef enum dc_deco_type_t {
	DC_DECO_NDL,
	DC_DECO_SAFETYSTOP,
	DC_DECO_DECOSTOP,
	DC_DECO_DEEPSTOP
} dc_deco_type_t;

typedef struct dc_salinity_t {
	dc_water_t type;
	double density;
} dc_salinity_t;

typedef enum dc_usage_t {
	DC_USAGE_NONE,
	DC_USAGE_OXYGEN,
	DC_USAGE_DILUENT,
	DC_USAGE_SIDEMOUNT,
} dc_usage_t;

typedef struct dc_gasmix_t {
	double helium;
	double oxygen;
	double nitrogen;
	dc_usage_t usage;
} dc_gasmix_t;

#define DC_SENSOR_NONE    0xFFFFFFFF
#define DC_GASMIX_UNKNOWN 0xFFFFFFFF

typedef enum dc_tankvolume_t {
    DC_TANKVOLUME_NONE,
    DC_TANKVOLUME_METRIC,
    DC_TANKVOLUME_IMPERIAL,
} dc_tankvolume_t;

/*
 * Tank volume
 *
 * There are two different ways to specify the volume of a tank. In the
 * metric system, the tank volume is specified as the water capacity,
 * while in the imperial system the tank volume is specified as the air
 * capacity at the surface (1 ATM) when the tank is filled at its
 * working pressure. Libdivecomputer will always convert the tank volume
 * to the metric representation, and indicate the original tank type:
 *
 * DC_TANKVOLUME_NONE: Tank volume is not available. Both the volume and
 * workpressure will be zero.
 *
 * DC_TANKVOLUME_METRIC: A metric tank. The workpressure is optional and
 * may be zero.
 *
 * DC_TANKVOLUME_IMPERIAL: An imperial tank. Both the volume and
 * workpressure are mandatory and always non-zero. The volume has been
 * converted from air capacity to water capacity. To calculate the
 * original air capacity again, multiply with the workpressure and
 * divide by 1 ATM (Vair = Vwater * Pwork / Patm).
 */

typedef struct dc_tank_t {
    unsigned int gasmix;  /* Gas mix index, or DC_GASMIX_UNKNOWN */
    dc_tankvolume_t type; /* Tank type */
    double volume;        /* Volume (liter) */
    double workpressure;  /* Work pressure (bar) */
    double beginpressure; /* Begin pressure (bar) */
    double endpressure;   /* End pressure (bar) */
    dc_usage_t usage;
} dc_tank_t;

typedef enum dc_decomodel_type_t {
	DC_DECOMODEL_NONE,
	DC_DECOMODEL_BUHLMANN,
	DC_DECOMODEL_VPM,
	DC_DECOMODEL_RGBM,
	DC_DECOMODEL_DCIEM,
} dc_decomodel_type_t;

/*
 * Decompression model
 *
 * The type field contains the decompression algorithm.
 *
 * The (optional) conservatism field contains the personal adjustment
 * setting of the algorithm. The exact interpretation depends on the
 * dive computer, but the default value (zero) will typically correspond
 * to the neutral setting, while a positive value is more conservative
 * and a negative value more aggressive.
 *
 * The (optional) params field contains the parameters of the algorithm:
 *
 * DC_DECOMODEL_BUHLMANN: The Gradient Factor (GF) parameters low and
 * high. For a pure Buhlmann algorithm (e.g. without GF enabled), both
 * values are 100. If GF are enabled, but the actual parameter values
 * are not available from the dive computer, both values are zero.
 */
typedef struct dc_decomodel_t {
	dc_decomodel_type_t type;
	int conservatism;
	union {
		struct {
			unsigned int high;
			unsigned int low;
		} gf;
	} params;
} dc_decomodel_t;

/*
 * GPS Location
 *
 * The latitude and longitude are in decimal degrees, and the (optional)
 * altitude in meters.
 */
typedef struct dc_location_t {
	double latitude;
	double longitude;
	double altitude;
} dc_location_t;

typedef union dc_sample_value_t {
	unsigned int time; /* Milliseconds */
	double depth;
	struct {
		unsigned int tank;
		double value;
	} pressure;
	double temperature;
	struct {
		unsigned int type;
		unsigned int time;
		unsigned int flags;
		unsigned int value;
	} event;
	unsigned int rbt;
	unsigned int heartbeat;
	unsigned int bearing;
	struct {
		unsigned int type;
		unsigned int size;
		const void *data;
	} vendor;
	double setpoint;
	struct {
		unsigned int sensor;
		double value;
	} ppo2;
	double cns;
	struct {
		unsigned int type;
		unsigned int time;
		double depth;
		unsigned int tts;
	} deco;
	unsigned int gasmix; /* Gas mix index */
} dc_sample_value_t;

typedef struct dc_parser_t dc_parser_t;

typedef void (*dc_sample_callback_t) (dc_sample_type_t type, const dc_sample_value_t *value, void *userdata);

dc_status_t
dc_parser_new (dc_parser_t **parser, dc_device_t *device, const unsigned char data[], size_t size);

dc_status_t
dc_parser_new2 (dc_parser_t **parser, dc_context_t *context, dc_descriptor_t *descriptor, const unsigned char data[], size_t size);

dc_family_t
dc_parser_get_type (dc_parser_t *parser);

dc_status_t
dc_parser_set_clock (dc_parser_t *parser, unsigned int devtime, dc_ticks_t systime);

dc_status_t
dc_parser_set_atmospheric (dc_parser_t *parser, double atmospheric);

dc_status_t
dc_parser_set_density (dc_parser_t *parser, double density);

dc_status_t
dc_parser_get_datetime (dc_parser_t *parser, dc_datetime_t *datetime);

dc_status_t
dc_parser_get_field (dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value);

dc_status_t
dc_parser_samples_foreach (dc_parser_t *parser, dc_sample_callback_t callback, void *userdata);

dc_status_t
dc_parser_destroy (dc_parser_t *parser);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_PARSER_H */
