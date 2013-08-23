/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#ifndef DC_IHEX_H
#define DC_IHEX_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_ihex_file_t dc_ihex_file_t;

typedef struct dc_ihex_entry_t {
	unsigned int type;
	unsigned int address;
	unsigned int length;
	unsigned char data[255];
} dc_ihex_entry_t;

dc_status_t
dc_ihex_file_open (dc_ihex_file_t **file, dc_context_t *context, const char *filename);

dc_status_t
dc_ihex_file_read (dc_ihex_file_t *file, dc_ihex_entry_t *entry);

dc_status_t
dc_ihex_file_reset (dc_ihex_file_t *file);

dc_status_t
dc_ihex_file_close (dc_ihex_file_t *file);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IHEX_H */
