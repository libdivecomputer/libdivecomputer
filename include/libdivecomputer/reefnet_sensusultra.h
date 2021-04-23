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

#ifndef DC_REEFNET_SENSUSULTRA_H
#define DC_REEFNET_SENSUSULTRA_H

#include "common.h"
#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define REEFNET_SENSUSULTRA_USER_SIZE 16384
#define REEFNET_SENSUSULTRA_HANDSHAKE_SIZE 24
#define REEFNET_SENSUSULTRA_SENSE_SIZE 6

typedef enum reefnet_sensusultra_parameter_t {
	REEFNET_SENSUSULTRA_PARAMETER_INTERVAL,
	REEFNET_SENSUSULTRA_PARAMETER_THRESHOLD,
	REEFNET_SENSUSULTRA_PARAMETER_ENDCOUNT,
	REEFNET_SENSUSULTRA_PARAMETER_AVERAGING
} reefnet_sensusultra_parameter_t;

dc_status_t
reefnet_sensusultra_device_get_handshake (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
reefnet_sensusultra_device_read_user (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
reefnet_sensusultra_device_write_user (dc_device_t *device, const unsigned char data[], unsigned int size);

dc_status_t
reefnet_sensusultra_device_write_parameter (dc_device_t *device, reefnet_sensusultra_parameter_t parameter, unsigned int value);

dc_status_t
reefnet_sensusultra_device_sense (dc_device_t *device, unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_REEFNET_SENSUSULTRA_H */
