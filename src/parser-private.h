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

#ifndef PARSER_PRIVATE_H
#define PARSER_PRIVATE_H

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct parser_t;
struct parser_backend_t;

typedef struct parser_backend_t parser_backend_t;

struct parser_t {
	const parser_backend_t *backend;
	const unsigned char *data;
	unsigned int size;
};

struct parser_backend_t {
	parser_type_t type;

	parser_status_t (*set_data) (parser_t *parser, const unsigned char *data, unsigned int size);

	parser_status_t (*datetime) (parser_t *parser, dc_datetime_t *datetime);

	parser_status_t (*field) (parser_t *parser, parser_field_type_t type, unsigned int flags, void *value);

	parser_status_t (*samples_foreach) (parser_t *parser, sample_callback_t callback, void *userdata);

	parser_status_t (*destroy) (parser_t *parser);
};

void
parser_init (parser_t *parser, const parser_backend_t *backend);

typedef struct sample_statistics_t {
	unsigned int divetime;
	double maxdepth;
} sample_statistics_t;

#define SAMPLE_STATISTICS_INITIALIZER {0, 0.0}

void
sample_statistics_cb (parser_sample_type_t type, parser_sample_value_t value, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PARSER_PRIVATE_H */
