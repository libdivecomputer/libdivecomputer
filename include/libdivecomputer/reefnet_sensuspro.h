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

#ifndef DC_REEFNET_SENSUSPRO_H
#define DC_REEFNET_SENSUSPRO_H

#include "common.h"
#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define REEFNET_SENSUSPRO_HANDSHAKE_SIZE 10

dc_status_t
reefnet_sensuspro_device_get_handshake (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
reefnet_sensuspro_device_write_interval (dc_device_t *device, unsigned char interval);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_REEFNET_SENSUSPRO_H */
