/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#ifndef DC_BLUETOOTH_H
#define DC_BLUETOOTH_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>
#include <libdivecomputer/iostream.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Bluetooth address (48 bits).
 */
#if defined (_WIN32) && !defined (__GNUC__)
typedef unsigned __int64 dc_bluetooth_address_t;
#else
typedef unsigned long long dc_bluetooth_address_t;
#endif

/**
 * Bluetooth enumeration callback.
 *
 * @param[in]  address   The bluetooth device address.
 * @param[in]  name      The bluetooth device name.
 * @param[in]  userdata  The user data pointer.
 */
typedef void (*dc_bluetooth_callback_t) (dc_bluetooth_address_t address, const char *name, void *userdata);

/**
 * Open an bluetooth connection.
 *
 * @param[out]  iostream   A location to store the bluetooth connection.
 * @param[in]   context    A valid context object.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_open (dc_iostream_t **iostream, dc_context_t *context);

/**
 * Enumerate the bluetooth devices.
 *
 * @param[in]  iostream   A valid bluetooth connection.
 * @param[in]  callback   The callback function to call.
 * @param[in]  userdata   User data to pass to the callback function.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_discover (dc_iostream_t *iostream, dc_bluetooth_callback_t callback, void *userdata);

/**
 * Connect to an bluetooth device.
 *
 * @param[in]   iostream   A valid bluetooth connection.
 * @param[in]   address    The bluetooth device address.
 * @param[in]   port       The bluetooth port number.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_connect (dc_iostream_t *iostream, dc_bluetooth_address_t address, unsigned int port);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BLUETOOTH_H */
