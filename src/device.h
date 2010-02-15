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

#ifndef DEVICE_H
#define DEVICE_H

#include "buffer.h"
#include "datetime.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum device_type_t {
	DEVICE_TYPE_NULL = 0,
	DEVICE_TYPE_SUUNTO_SOLUTION,
	DEVICE_TYPE_SUUNTO_EON,
	DEVICE_TYPE_SUUNTO_VYPER,
	DEVICE_TYPE_SUUNTO_VYPER2,
	DEVICE_TYPE_SUUNTO_D9,
	DEVICE_TYPE_REEFNET_SENSUS,
	DEVICE_TYPE_REEFNET_SENSUSPRO,
	DEVICE_TYPE_REEFNET_SENSUSULTRA,
	DEVICE_TYPE_UWATEC_ALADIN,
	DEVICE_TYPE_UWATEC_MEMOMOUSE,
	DEVICE_TYPE_UWATEC_SMART,
	DEVICE_TYPE_OCEANIC_ATOM2,
	DEVICE_TYPE_OCEANIC_VEO250,
	DEVICE_TYPE_OCEANIC_VTPRO,
	DEVICE_TYPE_MARES_NEMO,
	DEVICE_TYPE_MARES_PUCK,
	DEVICE_TYPE_HW_OSTC,
	DEVICE_TYPE_CRESSI_EDY
} device_type_t;

typedef enum device_status_t {
	DEVICE_STATUS_SUCCESS = 0,
	DEVICE_STATUS_UNSUPPORTED = -1,
	DEVICE_STATUS_TYPE_MISMATCH = -2,
	DEVICE_STATUS_ERROR = -3,
	DEVICE_STATUS_IO = -4,
	DEVICE_STATUS_TIMEOUT = -5,
	DEVICE_STATUS_PROTOCOL = -6,
	DEVICE_STATUS_MEMORY = -7,
	DEVICE_STATUS_CANCELLED = -8
} device_status_t;

typedef enum device_event_t {
	DEVICE_EVENT_WAITING = (1 << 0),
	DEVICE_EVENT_PROGRESS = (1 << 1),
	DEVICE_EVENT_DEVINFO = (1 << 2),
	DEVICE_EVENT_CLOCK = (1 << 3)
} device_event_t;

typedef struct device_t device_t;

typedef struct device_progress_t {
	unsigned int current;
	unsigned int maximum;
} device_progress_t;

typedef struct device_devinfo_t {
	unsigned int model;
	unsigned int firmware;
	unsigned int serial;
} device_devinfo_t;

typedef struct device_clock_t {
	unsigned int devtime;
	dc_ticks_t systime;
} device_clock_t;

typedef int (*device_cancel_callback_t) (void *userdata);

typedef void (*device_event_callback_t) (device_t *device, device_event_t event, const void *data, void *userdata);

typedef int (*dive_callback_t) (const unsigned char *data, unsigned int size, const unsigned char *fingerprint, unsigned int fsize, void *userdata);

device_type_t device_get_type (device_t *device);

device_status_t device_set_cancel (device_t *device, device_cancel_callback_t callback, void *userdata);

device_status_t device_set_events (device_t *device, unsigned int events, device_event_callback_t callback, void *userdata);

device_status_t device_set_fingerprint (device_t *device, const unsigned char data[], unsigned int size);

device_status_t device_version (device_t *device, unsigned char data[], unsigned int size);

device_status_t device_read (device_t *device, unsigned int address, unsigned char data[], unsigned int size);

device_status_t device_write (device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

device_status_t device_dump (device_t *device, dc_buffer_t *buffer);

device_status_t device_foreach (device_t *device, dive_callback_t callback, void *userdata);

device_status_t device_close (device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DEVICE_H */
