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

#include <libdivecomputer/iostream.h>

#include "device-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ID_SERIAL    0x8010
#define ID_FIRMWARE  0x8011
#define ID_LOGUPLOAD 0x8021
#define ID_HARDWARE  0x8050

#define ID_TIME_LOCAL  0x9030
#define ID_TIME_UTC    0x9031
#define ID_TIME_OFFSET 0x9032
#define ID_TIME_DST    0x9033

#define PREDATOR 2
#define PETREL   3
#define PETREL2  PETREL
#define NERD     4
#define PERDIX   5
#define PERDIXAI 6
#define NERD2    7
#define TERIC    8
#define PEREGRINE 9
#define PETREL3  10
#define PERDIX2  11
#define TERN     12

#define NSTEPS    10000
#define STEP(i,n) ((NSTEPS * (i) + (n) / 2) / (n))

typedef struct shearwater_common_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
} shearwater_common_device_t;

dc_status_t
shearwater_common_setup (shearwater_common_device_t *device, dc_context_t *context, dc_iostream_t *iostream);

dc_status_t
shearwater_common_transfer (shearwater_common_device_t *device, const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize, unsigned int *actual);

dc_status_t
shearwater_common_download (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int address, unsigned int size, unsigned int compression, dc_event_progress_t *progress);

dc_status_t
shearwater_common_rdbi (shearwater_common_device_t *device, unsigned int id, unsigned char data[], unsigned int size);

dc_status_t
shearwater_common_wdbi (shearwater_common_device_t *device, unsigned int id, const unsigned char data[], unsigned int size);

dc_status_t
shearwater_common_timesync_local (shearwater_common_device_t *device, const dc_datetime_t *datetime);

dc_status_t
shearwater_common_timesync_utc (shearwater_common_device_t *device, const dc_datetime_t *datetime);

unsigned int
shearwater_common_get_model (shearwater_common_device_t *device, unsigned int hardware);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SHEARWATER_COMMON_H */
