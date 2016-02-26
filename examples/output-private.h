/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#ifndef DCTOOL_OUTPUT_PRIVATE_H
#define DCTOOL_OUTPUT_PRIVATE_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/parser.h>

#include "output.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dctool_output_vtable_t dctool_output_vtable_t;

struct dctool_output_t {
	const dctool_output_vtable_t *vtable;
	unsigned int number;
};

struct dctool_output_vtable_t {
	size_t size;

	dc_status_t (*write) (dctool_output_t *output, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize);

	dc_status_t (*free) (dctool_output_t *output);
};

dctool_output_t *
dctool_output_allocate (const dctool_output_vtable_t *vtable);

void
dctool_output_deallocate (dctool_output_t *output);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DCTOOL_OUTPUT_PRIVATE_H */
