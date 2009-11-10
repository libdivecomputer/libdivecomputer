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

#ifndef SUUNTO_COMMON2_H
#define SUUNTO_COMMON2_H

#include "device-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct suunto_common2_device_t {
	device_t base;
	unsigned char fingerprint[7];
} suunto_common2_device_t;

typedef struct suunto_common2_device_backend_t {
	device_backend_t base;
	device_status_t (*packet) (device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
} suunto_common2_device_backend_t;

void
suunto_common2_device_init (suunto_common2_device_t *device, const suunto_common2_device_backend_t *backend);

device_status_t
suunto_common2_device_set_fingerprint (device_t *device, const unsigned char data[], unsigned int size);

device_status_t
suunto_common2_device_version (device_t *device, unsigned char data[], unsigned int size);

device_status_t
suunto_common2_device_read (device_t *device, unsigned int address, unsigned char data[], unsigned int size);

device_status_t
suunto_common2_device_write (device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

device_status_t
suunto_common2_device_dump (device_t *device, dc_buffer_t *buffer);

device_status_t
suunto_common2_device_foreach (device_t *device, dive_callback_t callback, void *userdata);

device_status_t
suunto_common2_device_reset_maxdepth (device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_COMMON2_H */
