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

#ifndef DC_IRDA_H
#define DC_IRDA_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing an IrDA connection.
 */
typedef struct dc_irda_t dc_irda_t;

/**
 * IrDA enumeration callback.
 *
 * @param[in]  address   The IrDA device address.
 * @param[in]  name      The IrDA device name.
 * @param[in]  charset   The IrDA device character set.
 * @param[in]  hints     The IrDA device hints.
 * @param[in]  userdata  The user data pointer.
 */
typedef void (*dc_irda_callback_t) (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata);

/**
 * Open an IrDA connection.
 *
 * @param[out]  irda     A location to store the IrDA connection.
 * @param[in]   context  A valid context object.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_open (dc_irda_t **irda, dc_context_t *context);

/**
 * Close the IrDA connection and free all resources.
 *
 * @param[in]  irda  A valid IrDA connection.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_close (dc_irda_t *irda);

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
 * @param[in]  irda     A valid IrDA connection.
 * @param[in]  timeout  The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_set_timeout (dc_irda_t *irda, int timeout);

/**
 * Enumerate the IrDA devices.
 *
 * @param[in]  irda      A valid IrDA connection.
 * @param[in]  callback  The callback function to call.
 * @param[in]  userdata  User data to pass to the callback function.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_discover (dc_irda_t *irda, dc_irda_callback_t callback, void *userdata);

/**
 * Connect to an IrDA device.
 *
 * @param[in]   irda     A valid IrDA connection.
 * @param[in]   address  The IrDA device address.
 * @param[in]   name     The IrDA service name.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_connect_name (dc_irda_t *irda, unsigned int address, const char *name);

/**
 * Connect to an IrDA device.
 *
 * @param[in]   irda     A valid IrDA connection.
 * @param[in]   address  The IrDA device address.
 * @param[in]   lsap     The IrDA LSAP number.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_connect_lsap (dc_irda_t *irda, unsigned int address, unsigned int lsap);

/**
 * Query the number of available bytes in the input buffer.
 *
 * @param[in]   irda    A valid IrDA connection.
 * @param[out]  value   A location to store the number of bytes in the
 *                      input buffer.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_get_available (dc_irda_t *irda, size_t *value);

/**
 * Read data from the IrDA connection.
 *
 * @param[in]  irda    A valid IrDA connection.
 * @param[out] data    The memory buffer to read the data into.
 * @param[in]  size    The number of bytes to read.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_read (dc_irda_t *irda, void *data, size_t size, size_t *actual);

/**
 * Write data to the IrDA connection.
 *
 * @param[in]  irda    A valid IrDA connection.
 * @param[in]  data    The memory buffer to write the data from.
 * @param[in]  size    The number of bytes to write.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_write (dc_irda_t *irda, const void *data, size_t size, size_t *actual);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IRDA_H */
