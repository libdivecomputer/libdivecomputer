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

#ifndef DCTOOL_OUTPUT_H
#define DCTOOL_OUTPUT_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/parser.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dctool_output_t dctool_output_t;

typedef enum dctool_units_t {
	DCTOOL_UNITS_METRIC,
	DCTOOL_UNITS_IMPERIAL
} dctool_units_t;

dctool_output_t *
dctool_xml_output_new (const char *filename, dctool_units_t units);

dctool_output_t *
dctool_raw_output_new (const char *template);

dc_status_t
dctool_output_write (dctool_output_t *output, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize);

dc_status_t
dctool_output_free (dctool_output_t *output);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DCTOOL_OUTPUT_H */
