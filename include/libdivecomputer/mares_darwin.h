/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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

#ifndef MARES_DARWIN_H
#define MARES_DARWIN_H

#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

device_status_t
mares_darwin_device_open (device_t **device, const char *name, unsigned int model);

device_status_t
mares_darwin_extract_dives (device_t *device, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

parser_status_t
mares_darwin_parser_create (parser_t **parser, unsigned int model);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* MARES_DARWIN_H */
