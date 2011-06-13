/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
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

#ifndef SUUNTO_VYPER_H
#define SUUNTO_VYPER_H

#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define SUUNTO_VYPER_MEMORY_SIZE 0x2000
#define SUUNTO_VYPER_PACKET_SIZE 32

device_status_t
suunto_vyper_device_open (device_t **device, const char* name);

device_status_t
suunto_vyper_device_set_delay (device_t *device, unsigned int delay);

device_status_t
suunto_vyper_device_read_dive (device_t *device, dc_buffer_t *buffer, int init);

device_status_t
suunto_vyper_extract_dives (device_t *device, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

parser_status_t
suunto_vyper_parser_create (parser_t **parser);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER_H */
