/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#ifndef ZEAGLE_N2ITION3_H
#define ZEAGLE_N2ITION3_H

#include "context.h"
#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
zeagle_n2ition3_device_open (dc_device_t **device, dc_context_t *context, const char *name);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* ZEAGLE_N2ITION3_H */
