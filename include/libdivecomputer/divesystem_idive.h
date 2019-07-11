/*
 * libdivecomputer
 *
 * Copyright (C) 2019 Jef Driesen
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

#ifndef DC_DIVESYSTEM_IDIVE_H
#define DC_DIVESYSTEM_IDIVE_H

#include "common.h"
#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
divesystem_idive_device_fwupdate (dc_device_t *abstract, const char *filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_DIVESYSTEM_IDIVE_H */
