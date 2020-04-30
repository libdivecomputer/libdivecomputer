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

#ifndef DC_DESCRIPTOR_PRIVATE_H
#define DC_DESCRIPTOR_PRIVATE_H

#include <libdivecomputer/descriptor.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_usb_desc_t {
	unsigned short vid;
	unsigned short pid;
} dc_usb_desc_t;

typedef struct dc_usb_params_t {
	unsigned int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
} dc_usb_params_t;

int
dc_descriptor_filter (dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata, void *params);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_DESCRIPTOR_PRIVATE_H */
