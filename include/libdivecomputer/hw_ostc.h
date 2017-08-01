/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#ifndef DC_HW_OSTC_H
#define DC_HW_OSTC_H

#include "common.h"
#include "device.h"
#include "datetime.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define HW_OSTC_MD2HASH_SIZE 18
#define HW_OSTC_EEPROM_SIZE  256

typedef enum hw_ostc_format_t {
	HW_OSTC_FORMAT_RAW,
	HW_OSTC_FORMAT_RGB16,
	HW_OSTC_FORMAT_RGB24
} hw_ostc_format_t;

dc_status_t
hw_ostc_device_md2hash (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
hw_ostc_device_eeprom_read (dc_device_t *device, unsigned int bank, unsigned char data[], unsigned int size);

dc_status_t
hw_ostc_device_eeprom_write (dc_device_t *device, unsigned int bank, const unsigned char data[], unsigned int size);

dc_status_t
hw_ostc_device_reset (dc_device_t *device);

dc_status_t
hw_ostc_device_screenshot (dc_device_t *device, dc_buffer_t *buffer, hw_ostc_format_t format);

dc_status_t
hw_ostc_device_fwupdate (dc_device_t *abstract, const char *filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_HW_OSTC_H */
