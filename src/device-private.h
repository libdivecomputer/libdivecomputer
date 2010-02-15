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

#ifndef DEVICE_PRIVATE_H
#define DEVICE_PRIVATE_H

#include <limits.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DEVICE_PROGRESS_INITIALIZER {0, UINT_MAX}

struct device_t;
struct device_backend_t;

typedef struct device_backend_t device_backend_t;

struct device_t {
	const device_backend_t *backend;
	// Event notifications.
	unsigned int event_mask;
	device_event_callback_t event_callback;
	void *event_userdata;
	// Cancellation support.
	device_cancel_callback_t cancel_callback;
	void *cancel_userdata;
};

struct device_backend_t {
    device_type_t type;

	device_status_t (*set_fingerprint) (device_t *device, const unsigned char data[], unsigned int size);

	device_status_t (*version) (device_t *device, unsigned char data[], unsigned int size);

	device_status_t (*read) (device_t *device, unsigned int address, unsigned char data[], unsigned int size);

	device_status_t (*write) (device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

	device_status_t (*dump) (device_t *device, dc_buffer_t *buffer);

	device_status_t (*foreach) (device_t *device, dive_callback_t callback, void *userdata);

	device_status_t (*close) (device_t *device);
};

void
device_init (device_t *device, const device_backend_t *backend);

void
device_event_emit (device_t *device, device_event_t event, const void *data);

int
device_is_cancelled (device_t *device);

device_status_t
device_dump_read (device_t *device, unsigned char data[], unsigned int size, unsigned int blocksize);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DEVICE_PRIVATE_H */
