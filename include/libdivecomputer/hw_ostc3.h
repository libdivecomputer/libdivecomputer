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

#ifndef DC_HW_OSTC3_H
#define DC_HW_OSTC3_H

#include "common.h"
#include "device.h"
#include "datetime.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define HW_OSTC3_DISPLAY_SIZE    16
#define HW_OSTC3_CUSTOMTEXT_SIZE 60

dc_status_t
hw_ostc3_device_version (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
hw_ostc3_device_hardware (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
hw_ostc3_device_display (dc_device_t *device, const char *text);

dc_status_t
hw_ostc3_device_customtext (dc_device_t *device, const char *text);

dc_status_t
hw_ostc3_device_config_read (dc_device_t *abstract, unsigned int config, unsigned char data[], unsigned int size);

dc_status_t
hw_ostc3_device_config_write (dc_device_t *abstract, unsigned int config, const unsigned char data[], unsigned int size);

dc_status_t
hw_ostc3_device_config_reset (dc_device_t *abstract);

dc_status_t
hw_ostc3_device_fwupdate (dc_device_t *abstract, const char *filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_HW_OSTC3_H */
