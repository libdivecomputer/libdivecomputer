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

#ifndef OCEANIC_COMMON_H
#define OCEANIC_COMMON_H

#include "device-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define PAGESIZE 0x10
#define FPMAXSIZE 0x20

#define OCEANIC_COMMON_MATCH(version,patterns) \
	oceanic_common_match ((version), (patterns), \
	sizeof (patterns) / sizeof *(patterns))

typedef struct oceanic_common_layout_t {
	// Memory size.
	unsigned int memsize;
	// Device info.
	unsigned int cf_devinfo;
	// Ringbuffer pointers.
	unsigned int cf_pointers;
	// Logbook ringbuffer.
	unsigned int rb_logbook_begin;
	unsigned int rb_logbook_end;
	unsigned int rb_logbook_entry_size;
	// Profile ringbuffer
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	// The pointer mode indicates how the global ringbuffer pointers
	// should be interpreted (a first/last or a begin/end pair), and
	// how the profile pointers are stored in each logbook entry (two
	// 12-bit values or two 16-bit values with each 4 bits padding).
	unsigned int pt_mode_global;
	unsigned int pt_mode_logbook;
} oceanic_common_layout_t;

typedef struct oceanic_common_device_t {
	dc_device_t base;
	unsigned char version[PAGESIZE];
	unsigned char fingerprint[FPMAXSIZE];
	const oceanic_common_layout_t *layout;
	unsigned int multipage;
} oceanic_common_device_t;

typedef unsigned char oceanic_common_version_t[PAGESIZE + 1];

int
oceanic_common_match (const unsigned char *version, const oceanic_common_version_t patterns[], unsigned int n);

void
oceanic_common_device_init (oceanic_common_device_t *device);

dc_status_t
oceanic_common_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);

dc_status_t
oceanic_common_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);

dc_status_t
oceanic_common_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* OCEANIC_COMMON_H */
