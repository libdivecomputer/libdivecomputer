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

#ifndef SUUNTO_COMMON_H
#define SUUNTO_COMMON_H

#include "device-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct suunto_common_device_t {
	dc_device_t base;
	unsigned char fingerprint[5];
} suunto_common_device_t;

typedef struct suunto_common_layout_t {
	// End-of-profile marker
	unsigned int eop;
	// Profile ringbuffer
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	// Fingerprint
	unsigned int fp_offset;
	// Peek
	unsigned int peek;
} suunto_common_layout_t;

void
suunto_common_device_init (suunto_common_device_t *device);

dc_status_t
suunto_common_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);

dc_status_t
suunto_common_extract_dives (suunto_common_device_t *device, const suunto_common_layout_t *layout, const unsigned char data[], dc_dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_COMMON_H */
