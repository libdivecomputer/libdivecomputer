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

#ifndef DC_SERIAL_H
#define DC_SERIAL_H

#include "common.h"
#include "context.h"
#include "iostream.h"
#include "iterator.h"
#include "descriptor.h"
#include "ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a serial device.
 */
typedef struct dc_serial_device_t dc_serial_device_t;

/**
 * Get the device node of the serial device.
 *
 * @param[in]  device  A valid serial device.
 */
const char *
dc_serial_device_get_name (dc_serial_device_t *device);

/**
 * Destroy the serial device and free all resources.
 *
 * @param[in]  device  A valid serial device.
 */
void
dc_serial_device_free (dc_serial_device_t *device);

/**
 * Create an iterator to enumerate the serial devices.
 *
 * @param[out] iterator    A location to store the iterator.
 * @param[in]  context     A valid context object.
 * @param[in]  descriptor  A valid device descriptor or NULL.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_iterator_new (dc_iterator_t **iterator, dc_context_t *context, dc_descriptor_t *descriptor);

/**
 * Open a serial connection.
 *
 * @param[out]  iostream A location to store the serial connection.
 * @param[in]   context  A valid context object.
 * @param[in]   name     The name of the device node.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_open (dc_iostream_t **iostream, dc_context_t *context, const char *name);

/**
 * Set the receive latency in milliseconds.
 *
 * The effect of this setting is highly platform and driver specific. On
 * Windows it does nothing at all, on Linux it controls the low latency
 * flag (e.g. only zero vs non-zero latency), and on Mac OS X it sets
 * the receive latency as requested.
 */
#define DC_IOCTL_SERIAL_SET_LATENCY DC_IOCTL_IOW('s', 0, sizeof(unsigned int))

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_SERIAL_H */
