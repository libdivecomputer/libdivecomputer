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

#ifndef REEFNET_SENSUSULTRA_H
#define REEFNET_SENSUSULTRA_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define REEFNET_SENSUSULTRA_PACKET_SIZE 512
#define REEFNET_SENSUSULTRA_MEMORY_USER_SIZE 16384 /* 32 PAGES */
#define REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE 2080768 /* 4064 PAGES */
#define REEFNET_SENSUSULTRA_MEMORY_SIZE 2097152 /* USER + DATA */
#define REEFNET_SENSUSULTRA_HANDSHAKE_SIZE 24
#define REEFNET_SENSUSULTRA_SENSE_SIZE 6

device_status_t
reefnet_sensusultra_device_open (device_t **device, const char* name);

device_status_t
reefnet_sensusultra_device_set_maxretries (device_t *device, unsigned int maxretries);

device_status_t
reefnet_sensusultra_device_set_timestamp (device_t *device, unsigned int timestamp);

device_status_t
reefnet_sensusultra_device_read_user (device_t *device, unsigned char *data, unsigned int size);

device_status_t
reefnet_sensusultra_device_write_user (device_t *device, const unsigned char *data, unsigned int size);

device_status_t
reefnet_sensusultra_device_write_interval (device_t *device, unsigned int value);

device_status_t
reefnet_sensusultra_device_write_threshold (device_t *device, unsigned int value);

device_status_t
reefnet_sensusultra_device_write_endcount (device_t *device, unsigned int value);

device_status_t
reefnet_sensusultra_device_write_averaging (device_t *device, unsigned int value);

device_status_t
reefnet_sensusultra_device_sense (device_t *device, unsigned char *data, unsigned int size);

device_status_t
reefnet_sensusultra_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata, unsigned int timestamp);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* REEFNET_SENSUSULTRA_H */
