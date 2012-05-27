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
	DC_SAMPLE_VENDOR
} dc_sample_type_t;

typedef enum dc_field_type_t {
	DC_FIELD_DIVETIME,
	DC_FIELD_MAXDEPTH,
	DC_FIELD_AVGDEPTH,
	DC_FIELD_GASMIX_COUNT,
	DC_FIELD_GASMIX
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
	SAMPLE_EVENT_GASCHANGE,
	SAMPLE_EVENT_SAFETYSTOP_VOLUNTARY,
	SAMPLE_EVENT_SAFETYSTOP_MANDATORY,
	SAMPLE_EVENT_DEEPSTOP,
	SAMPLE_EVENT_CEILING_SAFETYSTOP,
	SAMPLE_EVENT_UNKNOWN,
	SAMPLE_EVENT_DIVETIME,
	SAMPLE_EVENT_MAXDEPTH,
	SAMPLE_EVENT_OLF,
	SAMPLE_EVENT_PO2,
	SAMPLE_EVENT_AIRTIME,
	SAMPLE_EVENT_RGBM,
	SAMPLE_EVENT_HEADING,
	SAMPLE_EVENT_TISSUELEVEL
} parser_sample_event_t;

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

typedef struct dc_gasmix_t {
	double helium;
	double oxygen;
	double nitrogen;
} dc_gasmix_t;

typedef union dc_sample_value_t {
	unsigned int time;
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
} dc_sample_value_t;

typedef struct dc_parser_t dc_parser_t;

typedef void (*dc_sample_callback_t) (dc_sample_type_t type, dc_sample_value_t value, void *userdata);

dc_status_t
dc_parser_new (dc_parser_t **parser, dc_device_t *device);

dc_family_t
dc_parser_get_type (dc_parser_t *parser);

dc_status_t
dc_parser_set_data (dc_parser_t *parser, const unsigned char *data, unsigned int size);

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
