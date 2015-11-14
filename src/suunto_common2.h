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

typedef struct suunto_common2_layout_t {
	// Memory size.
	unsigned int memsize;
	// Fingerprint
	unsigned int fingerprint;
	// Serial number.
	unsigned int serial;
	// Profile ringbuffer
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
} suunto_common2_layout_t;

typedef struct suunto_common2_device_t {
	dc_device_t base;
	const suunto_common2_layout_t *layout;
	unsigned char version[4];
	unsigned char fingerprint[7];
} suunto_common2_device_t;

typedef struct suunto_common2_device_vtable_t {
	dc_device_vtable_t base;
	dc_status_t (*packet) (dc_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
} suunto_common2_device_vtable_t;

void
suunto_common2_device_init (suunto_common2_device_t *device);

dc_status_t
suunto_common2_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);

dc_status_t
suunto_common2_device_version (dc_device_t *device, unsigned char data[], unsigned int size);

dc_status_t
suunto_common2_device_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size);

dc_status_t
suunto_common2_device_write (dc_device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

dc_status_t
suunto_common2_device_dump (dc_device_t *device, dc_buffer_t *buffer);

dc_status_t
suunto_common2_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

dc_status_t
suunto_common2_device_reset_maxdepth (dc_device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_COMMON2_H */
