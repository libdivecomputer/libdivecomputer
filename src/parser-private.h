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

#define DEF_DENSITY_FRESH 1000.0
#define DEF_DENSITY_SALT  1025.0
#define DEF_ATMOSPHERIC   ATM

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dc_parser_t;
struct dc_parser_vtable_t;

typedef struct dc_parser_vtable_t dc_parser_vtable_t;

struct dc_parser_t {
	const dc_parser_vtable_t *vtable;
	dc_context_t *context;
	unsigned char *data;
	unsigned int size;
};

struct dc_parser_vtable_t {
	size_t size;

	dc_family_t type;

	dc_status_t (*set_clock) (dc_parser_t *parser, unsigned int devtime, dc_ticks_t systime);

	dc_status_t (*set_atmospheric) (dc_parser_t *parser, double atmospheric);

	dc_status_t (*set_density) (dc_parser_t *parser, double density);

	dc_status_t (*datetime) (dc_parser_t *parser, dc_datetime_t *datetime);

	dc_status_t (*field) (dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value);

	dc_status_t (*samples_foreach) (dc_parser_t *parser, dc_sample_callback_t callback, void *userdata);

	dc_status_t (*destroy) (dc_parser_t *parser);
};

dc_parser_t *
dc_parser_allocate (dc_context_t *context, const dc_parser_vtable_t *vtable, const unsigned char data[], size_t size);

void
dc_parser_deallocate (dc_parser_t *parser);

int
dc_parser_isinstance (dc_parser_t *parser, const dc_parser_vtable_t *vtable);

typedef struct sample_statistics_t {
	unsigned int divetime;
	double maxdepth;
} sample_statistics_t;

#define SAMPLE_STATISTICS_INITIALIZER {0, 0.0}

void
sample_statistics_cb (dc_sample_type_t type, const dc_sample_value_t *value, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PARSER_PRIVATE_H */
