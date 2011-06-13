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

#ifndef REEFNET_SENSUSPRO_H
#define REEFNET_SENSUSPRO_H

#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define REEFNET_SENSUSPRO_MEMORY_SIZE 56320
#define REEFNET_SENSUSPRO_HANDSHAKE_SIZE 10

device_status_t
reefnet_sensuspro_device_open (device_t **device, const char* name);

device_status_t
reefnet_sensuspro_device_set_timestamp (device_t *device, unsigned int timestamp);

device_status_t
reefnet_sensuspro_device_get_handshake (device_t *abstract, unsigned char data[], unsigned int size);

device_status_t
reefnet_sensuspro_device_write_interval (device_t *device, unsigned char interval);

device_status_t
reefnet_sensuspro_extract_dives (device_t *device, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

parser_status_t
reefnet_sensuspro_parser_create (parser_t **parser, unsigned int devtime, dc_ticks_t systime);

parser_status_t
reefnet_sensuspro_parser_set_calibration (parser_t *parser, double atmospheric, double hydrostatic);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* REEFNET_SENSUSPRO_H */
