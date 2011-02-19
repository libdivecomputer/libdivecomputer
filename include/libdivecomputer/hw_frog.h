/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#ifndef HW_FROG_H
#define HW_FROG_H

#include "device.h"
#include "parser.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
hw_frog_device_open (device_t **device, const char *name);

dc_status_t
hw_frog_device_clock (device_t *device, const dc_datetime_t *datetime);

dc_status_t
hw_frog_device_display (device_t *device, const char *text);

dc_status_t
hw_frog_device_customtext (device_t *abstract, const char *text);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* HW_FROG_H */
