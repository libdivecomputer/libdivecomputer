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

#include <libdivecomputer/context.h>
#include <libdivecomputer/device.h>

#include "common-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define EVENT_PROGRESS_INITIALIZER {0, UINT_MAX}

struct dc_device_t;
struct dc_device_vtable_t;

typedef struct dc_device_vtable_t dc_device_vtable_t;

struct dc_device_t {
	const dc_device_vtable_t *vtable;
	// Library context.
	dc_context_t *context;
	// Event notifications.
	unsigned int event_mask;
	dc_event_callback_t event_callback;
	void *event_userdata;
	// Cancellation support.
	dc_cancel_callback_t cancel_callback;
	void *cancel_userdata;
	// Cached events for the parsers.
	dc_event_devinfo_t devinfo;
	dc_event_clock_t clock;
};

struct dc_device_vtable_t {
	size_t size;

	dc_family_t type;

	dc_status_t (*set_fingerprint) (dc_device_t *device, const unsigned char data[], unsigned int size);

	dc_status_t (*read) (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size);

	dc_status_t (*write) (dc_device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

	dc_status_t (*dump) (dc_device_t *device, dc_buffer_t *buffer);

	dc_status_t (*foreach) (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

	dc_status_t (*timesync) (dc_device_t *device, const dc_datetime_t *datetime);

	dc_status_t (*close) (dc_device_t *device);
};

int
dc_device_isinstance (dc_device_t *device, const dc_device_vtable_t *vtable);

dc_device_t *
dc_device_allocate (dc_context_t *context, const dc_device_vtable_t *vtable);

void
dc_device_deallocate (dc_device_t *device);

void
device_event_emit (dc_device_t *device, dc_event_type_t event, const void *data);

int
device_is_cancelled (dc_device_t *device);

dc_status_t
device_dump_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size, unsigned int blocksize);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DEVICE_PRIVATE_H */
