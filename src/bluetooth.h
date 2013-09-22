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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a bluetooth connection.
 */
typedef struct dc_bluetooth_t dc_bluetooth_t;

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
 * @param[out]  bluetooth  A location to store the bluetooth connection.
 * @param[in]   context    A valid context object.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_open (dc_bluetooth_t **bluetooth, dc_context_t *context);

/**
 * Close the bluetooth connection and free all resources.
 *
 * @param[in]  bluetooth  A valid bluetooth connection.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_close (dc_bluetooth_t *bluetooth);

/**
 * Set the read timeout.
 *
 * There are three distinct modes available:
 *
 *  1. Blocking (timeout < 0):
 *
 *     The read operation is blocked until all the requested bytes have
 *     been received. If the requested number of bytes does not arrive,
 *     the operation will block forever.
 *
 *  2. Non-blocking (timeout == 0):
 *
 *     The read operation returns immediately with the bytes that have
 *     already been received, even if no bytes have been received.
 *
 *  3. Timeout (timeout > 0):
 *
 *     The read operation is blocked until all the requested bytes have
 *     been received. If the requested number of bytes does not arrive
 *     within the specified amount of time, the operation will return
 *     with the bytes that have already been received.
 *
 * @param[in]  bluetooth  A valid bluetooth connection.
 * @param[in]  timeout    The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_set_timeout (dc_bluetooth_t *bluetooth, int timeout);

/**
 * Enumerate the bluetooth devices.
 *
 * @param[in]  bluetooth  A valid bluetooth connection.
 * @param[in]  callback   The callback function to call.
 * @param[in]  userdata   User data to pass to the callback function.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_discover (dc_bluetooth_t *bluetooth, dc_bluetooth_callback_t callback, void *userdata);

/**
 * Connect to an bluetooth device.
 *
 * @param[in]   bluetooth  A valid bluetooth connection.
 * @param[in]   address    The bluetooth device address.
 * @param[in]   port       The bluetooth port number.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_connect (dc_bluetooth_t *bluetooth, dc_bluetooth_address_t address, unsigned int port);

/**
 * Query the number of available bytes in the input buffer.
 *
 * @param[in]   bluetooth  A valid bluetooth connection.
 * @param[out]  value      A location to store the number of bytes in
 *                         the input buffer.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_get_available (dc_bluetooth_t *bluetooth, size_t *value);

/**
 * Read data from the bluetooth connection.
 *
 * @param[in]  bluetooth  A valid bluetooth connection.
 * @param[out] data       The memory buffer to read the data into.
 * @param[in]  size       The number of bytes to read.
 * @param[out] actual     An (optional) location to store the actual
 *                        number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_read (dc_bluetooth_t *bluetooth, void *data, size_t size, size_t *actual);

/**
 * Write data to the bluetooth connection.
 *
 * @param[in]  bluetooth  A valid bluetooth connection.
 * @param[in]  data       The memory buffer to write the data from.
 * @param[in]  size       The number of bytes to write.
 * @param[out] actual     An (optional) location to store the actual
 *                        number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_write (dc_bluetooth_t *bluetooth, const void *data, size_t size, size_t *actual);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BLUETOOTH_H */
