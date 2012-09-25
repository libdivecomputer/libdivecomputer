/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#ifndef SHEARWATER_PREDATOR_H
#define SHEARWATER_PREDATOR_H

#include "context.h"
#include "device.h"
#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
shearwater_predator_device_open (dc_device_t **device, dc_context_t *context, const char *name);

dc_status_t
shearwater_predator_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

dc_status_t
shearwater_predator_parser_create (dc_parser_t **parser, dc_context_t *context);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SHEARWATER_PREDATOR_H */
