/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#ifndef DC_CUSTOM_H
#define DC_CUSTOM_H

#include "common.h"
#include "context.h"
#include "iostream.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_custom_cbs_t {
	dc_status_t (*set_timeout) (void *userdata, int timeout);
	dc_status_t (*set_break) (void *userdata, unsigned int value);
	dc_status_t (*set_dtr) (void *userdata, unsigned int value);
	dc_status_t (*set_rts) (void *userdata, unsigned int value);
	dc_status_t (*get_lines) (void *userdata, unsigned int *value);
	dc_status_t (*get_available) (void *userdata, size_t *value);
	dc_status_t (*configure) (void *userdata, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
	dc_status_t (*poll) (void *userdata, int timeout);
	dc_status_t (*read) (void *userdata, void *data, size_t size, size_t *actual);
	dc_status_t (*write) (void *userdata, const void *data, size_t size, size_t *actual);
	dc_status_t (*ioctl) (void *userdata, unsigned int request, void *data, size_t size);
	dc_status_t (*flush) (void *userdata);
	dc_status_t (*purge) (void *userdata, dc_direction_t direction);
	dc_status_t (*sleep) (void *userdata, unsigned int milliseconds);
	dc_status_t (*close) (void *userdata);
} dc_custom_cbs_t;

/**
 * Create a custom I/O stream.
 *
 * @param[out]  iostream   A location to store the custom I/O stream.
 * @param[in]   context    A valid context object.
 * @param[in]   callbacks  The callback functions to call.
 * @param[in]   userdata   User data to pass to the callback functions.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_custom_open (dc_iostream_t **iostream, dc_context_t *context, dc_transport_t transport, const dc_custom_cbs_t *callbacks, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_CUSTOM_H */
