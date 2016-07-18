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

#ifndef DC_USBHID_H
#define DC_USBHID_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a USB HID connection.
 */
typedef struct dc_usbhid_t dc_usbhid_t;

/**
 * Open a USB HID connection.
 *
 * @param[out]  usbhid   A location to store the USB HID connection.
 * @param[in]   context  A valid context object.
 * @param[in]   vid      The USB Vendor ID of the device.
 * @param[in]   pid      The USB Product ID of the device.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usbhid_open (dc_usbhid_t **usbhid, dc_context_t *context, unsigned int vid, unsigned int pid);

/**
 * Close the connection and free all resources.
 *
 * @param[in]  usbhid  A valid USB HID connection.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usbhid_close (dc_usbhid_t *usbhid);

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
 * @param[in]  usbhid   A valid USB HID connection.
 * @param[in]  timeout  The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usbhid_set_timeout (dc_usbhid_t *usbhid, int timeout);

/**
 * Read data from the USB HID connection.
 *
 * @param[in]  usbhid  A valid USB HID connection.
 * @param[out] data    The memory buffer to read the data into.
 * @param[in]  size    The number of bytes to read.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usbhid_read (dc_usbhid_t *usbhid, void *data, size_t size, size_t *actual);

/**
 * Write data to the USB HID connection.
 *
 * @param[in]  usbhid  A valid USB HID connection.
 * @param[in]  data    The memory buffer to write the data from.
 * @param[in]  size    The number of bytes to write.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usbhid_write (dc_usbhid_t *usbhid, const void *data, size_t size, size_t *actual);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_USBHID_H */
