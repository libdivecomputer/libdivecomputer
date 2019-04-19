/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#ifndef DC_IOSTREAM_PRIVATE_H
#define DC_IOSTREAM_PRIVATE_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>
#include <libdivecomputer/iostream.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_iostream_vtable_t dc_iostream_vtable_t;

struct dc_iostream_t {
	const dc_iostream_vtable_t *vtable;
	dc_context_t *context;
	dc_transport_t transport;
};

struct dc_iostream_vtable_t {
	size_t size;

	dc_status_t (*set_timeout) (dc_iostream_t *iostream, int timeout);

	dc_status_t (*set_break) (dc_iostream_t *iostream, unsigned int value);

	dc_status_t (*set_dtr) (dc_iostream_t *iostream, unsigned int value);

	dc_status_t (*set_rts) (dc_iostream_t *iostream, unsigned int value);

	dc_status_t (*get_lines) (dc_iostream_t *iostream, unsigned int *value);

	dc_status_t (*get_available) (dc_iostream_t *iostream, size_t *value);

	dc_status_t (*configure) (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);

	dc_status_t (*poll) (dc_iostream_t *iostream, int timeout);

	dc_status_t (*read) (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);

	dc_status_t (*write) (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);

	dc_status_t (*ioctl) (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);

	dc_status_t (*flush) (dc_iostream_t *iostream);

	dc_status_t (*purge) (dc_iostream_t *iostream, dc_direction_t direction);

	dc_status_t (*sleep) (dc_iostream_t *iostream, unsigned int milliseconds);

	dc_status_t (*close) (dc_iostream_t *iostream);
};

dc_iostream_t *
dc_iostream_allocate (dc_context_t *context, const dc_iostream_vtable_t *vtable, dc_transport_t transport);

void
dc_iostream_deallocate (dc_iostream_t *iostream);

int
dc_iostream_isinstance (dc_iostream_t *iostream, const dc_iostream_vtable_t *vtable);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IOSTREAM_PRIVATE_H */
