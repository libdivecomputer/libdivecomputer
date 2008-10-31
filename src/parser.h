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

#ifndef PARSER_H
#define PARSER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum parser_type_t {
	PARSER_TYPE_NULL = 0
} parser_type_t;

typedef enum parser_status_t {
	PARSER_STATUS_SUCCESS = 0,
	PARSER_STATUS_UNSUPPORTED = -1,
	PARSER_STATUS_TYPE_MISMATCH = -2,
	PARSER_STATUS_ERROR = -3,
	PARSER_STATUS_MEMORY = -7
} parser_status_t;

typedef enum parser_sample_type_t {
	SAMPLE_TYPE_TIME,
	SAMPLE_TYPE_DEPTH,
	SAMPLE_TYPE_PRESSURE,
	SAMPLE_TYPE_TEMPERATURE,
	SAMPLE_TYPE_EVENT,
	SAMPLE_TYPE_RBT,
	SAMPLE_TYPE_HEARTBEAT
} parser_sample_type_t;

typedef enum parser_sample_event_t {
	SAMPLE_EVENT_NONE
} parser_sample_event_t;

typedef enum parser_sample_flags_t {
	SAMPLE_FLAGS_NONE
} parser_sample_flags_t;

typedef union parser_sample_value_t {
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
} parser_sample_value_t;

typedef struct parser_t parser_t;

typedef void (*sample_callback_t) (parser_sample_type_t type, parser_sample_value_t value, void *userdata);

parser_type_t
parser_get_type (parser_t *device);

parser_status_t
parser_set_data (parser_t *parser, const unsigned char *data, unsigned int size);

parser_status_t
parser_samples_foreach (parser_t *parser, sample_callback_t callback, void *userdata);

parser_status_t
parser_destroy (parser_t *parser);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PARSER_H */
