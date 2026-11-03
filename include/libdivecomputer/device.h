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

#ifndef DC_DEVICE_H
#define DC_DEVICE_H

#include "common.h"
#include "context.h"
#include "descriptor.h"
#include "iostream.h"
#include "buffer.h"
#include "datetime.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dc_event_type_t {
	DC_EVENT_WAITING = (1 << 0),
	DC_EVENT_PROGRESS = (1 << 1),
	DC_EVENT_DEVINFO = (1 << 2),
	DC_EVENT_CLOCK = (1 << 3),
	DC_EVENT_VENDOR = (1 << 4)
} dc_event_type_t;

typedef struct dc_device_t dc_device_t;

typedef struct dc_event_progress_t {
	unsigned int current;
	unsigned int maximum;
} dc_event_progress_t;

typedef struct dc_event_devinfo_t {
	unsigned int model;
	unsigned int firmware;
	unsigned int serial;
} dc_event_devinfo_t;

typedef struct dc_event_clock_t {
	unsigned int devtime;
	dc_ticks_t systime;
} dc_event_clock_t;

typedef struct dc_event_vendor_t {
	const unsigned char *data;
	unsigned int size;
} dc_event_vendor_t;

typedef int (*dc_cancel_callback_t) (void *userdata);

typedef void (*dc_event_callback_t) (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata);

typedef int (*dc_dive_callback_t) (const unsigned char *data, unsigned int size, const unsigned char *fingerprint, unsigned int fsize, void *userdata);

/*
 * Dive selection (fork extension, not part of upstream libdivecomputer).
 *
 * A backend that honours the dive filter calls it once for every dive it
 * is about to download, before transferring the dive data. The entry
 * fingerprint has the same value and size as the fingerprint passed to
 * the dive callback for that dive. The fields bitmask tells which of the
 * optional fields are valid. The filter returns nonzero to download the
 * dive, or zero to skip it without transferring the dive data.
 */
typedef enum dc_dive_entry_field_t {
	DC_DIVE_ENTRY_DATETIME = (1 << 0)
} dc_dive_entry_field_t;

typedef struct dc_dive_entry_t {
	const unsigned char *fingerprint;
	unsigned int fsize;
	unsigned int fields;
	dc_datetime_t datetime;
} dc_dive_entry_t;

typedef int (*dc_dive_filter_t) (const dc_dive_entry_t *entry, void *userdata);

dc_status_t
dc_device_open (dc_device_t **out, dc_context_t *context, dc_descriptor_t *descriptor, dc_iostream_t *iostream);

dc_family_t
dc_device_get_type (dc_device_t *device);

dc_status_t
dc_device_set_cancel (dc_device_t *device, dc_cancel_callback_t callback, void *userdata);

dc_status_t
dc_device_set_events (dc_device_t *device, unsigned int events, dc_event_callback_t callback, void *userdata);

dc_status_t
dc_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);

/*
 * Install (or clear, with a NULL filter) the dive filter used by
 * dc_device_foreach. Returns DC_STATUS_UNSUPPORTED for device families
 * that do not honour a dive filter.
 */
dc_status_t
dc_device_set_dive_filter (dc_device_t *device, dc_dive_filter_t filter, void *userdata);

dc_status_t
dc_device_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size);

dc_status_t
dc_device_write (dc_device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

dc_status_t
dc_device_dump (dc_device_t *device, dc_buffer_t *buffer);

dc_status_t
dc_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

dc_status_t
dc_device_timesync (dc_device_t *device, const dc_datetime_t *datetime);

dc_status_t
dc_device_close (dc_device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_DEVICE_H */
