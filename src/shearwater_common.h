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

#ifndef SHEARWATER_COMMON_H
#define SHEARWATER_COMMON_H

#include "device-private.h"
#include "serial.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ID_SERIAL   0x8010
#define ID_FIRMWARE 0x8011
#define ID_HARDWARE 0x8050

#define PREDATOR 2
#define PETREL   3
#define NERD     4
#define PERDIX   5
#define PERDIXAI 6
#define NERD2    7

#define NSTEPS    10000
#define STEP(i,n) ((NSTEPS * (i) + (n) / 2) / (n))

typedef struct shearwater_common_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
} shearwater_common_device_t;

dc_status_t
shearwater_common_open (shearwater_common_device_t *device, dc_context_t *context, const char *name);

dc_status_t
shearwater_common_close (shearwater_common_device_t *device);

dc_status_t
shearwater_common_transfer (shearwater_common_device_t *device, const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize, unsigned int *actual);

dc_status_t
shearwater_common_download (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int address, unsigned int size, unsigned int compression, dc_event_progress_t *progress);

dc_status_t
shearwater_common_identifier (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int id);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SHEARWATER_COMMON_H */
