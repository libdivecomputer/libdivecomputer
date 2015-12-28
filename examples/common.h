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

dc_family_t
dctool_family_type (const char *name);

const char *
dctool_family_name (dc_family_t type);

dc_status_t
dctool_descriptor_search (dc_descriptor_t **out, const char *name, dc_family_t family, unsigned int model);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DCTOOL_COMMON_H */
