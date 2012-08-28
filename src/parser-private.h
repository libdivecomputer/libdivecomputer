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

#include <libdivecomputer/context.h>
#include <libdivecomputer/parser.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dc_parser_t;
struct parser_backend_t;

typedef struct parser_backend_t parser_backend_t;

struct dc_parser_t {
	const parser_backend_t *backend;
	dc_context_t *context;
	const unsigned char *data;
	unsigned int size;
};

struct parser_backend_t {
	dc_family_t type;

	dc_status_t (*set_data) (dc_parser_t *parser, const unsigned char *data, unsigned int size);

	dc_status_t (*datetime) (dc_parser_t *parser, dc_datetime_t *datetime);

	dc_status_t (*field) (dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value);

	dc_status_t (*samples_foreach) (dc_parser_t *parser, dc_sample_callback_t callback, void *userdata);

	dc_status_t (*destroy) (dc_parser_t *parser);
};

void
parser_init (dc_parser_t *parser, dc_context_t *context, const parser_backend_t *backend);

typedef struct sample_statistics_t {
	unsigned int divetime;
	double maxdepth;
} sample_statistics_t;

#define SAMPLE_STATISTICS_INITIALIZER {0, 0.0}

void
sample_statistics_cb (dc_sample_type_t type, dc_sample_value_t value, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PARSER_PRIVATE_H */
