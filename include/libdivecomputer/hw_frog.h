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

#ifndef DC_HW_FROG_H
#define DC_HW_FROG_H

#include "common.h"
#include "device.h"
#include "datetime.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define HW_FROG_DISPLAY_SIZE    15
#define HW_FROG_CUSTOMTEXT_SIZE 13

dc_status_t
hw_frog_device_version (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
hw_frog_device_display (dc_device_t *device, const char *text);

dc_status_t
hw_frog_device_customtext (dc_device_t *device, const char *text);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_HW_FROG_H */
