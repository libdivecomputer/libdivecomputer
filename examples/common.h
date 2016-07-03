/*
 * libdivecomputer
 *
 * Copyright (C) 2015 Jef Driesen
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

#ifndef DCTOOL_COMMON_H
#define DCTOOL_COMMON_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/device.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

const char *
dctool_errmsg (dc_status_t status);

dc_family_t
dctool_family_type (const char *name);

const char *
dctool_family_name (dc_family_t type);

unsigned int
dctool_family_model (dc_family_t type);

void
dctool_event_cb (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata);

dc_status_t
dctool_descriptor_search (dc_descriptor_t **out, const char *name, dc_family_t family, unsigned int model);

dc_buffer_t *
dctool_convert_hex2bin (const char *str);

void
dctool_file_write (const char *filename, dc_buffer_t *buffer);

dc_buffer_t *
dctool_file_read (const char *filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DCTOOL_COMMON_H */
