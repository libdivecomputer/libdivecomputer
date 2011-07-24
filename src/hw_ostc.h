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

#ifndef HW_OSTC_H
#define HW_OSTC_H

#include "device.h"
#include "parser.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum hw_ostc_format_t {
	HW_OSTC_FORMAT_RAW,
	HW_OSTC_FORMAT_RGB16,
	HW_OSTC_FORMAT_RGB24
} hw_ostc_format_t;

device_status_t
hw_ostc_device_open (device_t **device, const char* name);

device_status_t
hw_ostc_device_md2hash (device_t *abstract, unsigned char data[], unsigned int size);

device_status_t
hw_ostc_device_clock (device_t *abstract, const dc_datetime_t *datetime);

device_status_t
hw_ostc_device_eeprom_read (device_t *abstract, unsigned int bank, unsigned char data[], unsigned int size);

device_status_t
hw_ostc_device_eeprom_write (device_t *abstract, unsigned int bank, const unsigned char data[], unsigned int size);

device_status_t
hw_ostc_device_reset (device_t *abstract);

device_status_t
hw_ostc_device_screenshot (device_t *abstract, dc_buffer_t *buffer, hw_ostc_format_t format);

device_status_t
hw_ostc_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

parser_status_t
hw_ostc_parser_create (parser_t **parser);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* HW_OSTC_H */
