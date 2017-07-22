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

#include "common.h"
#include "context.h"
#include "iostream.h"
#include "iterator.h"
#include "descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * The minimum number of bytes (including the terminating null byte) for
 * formatting a bluetooth address as a string.
 */
#define DC_BLUETOOTH_SIZE 18

/**
 * Bluetooth address (48 bits).
 */
#if defined (_WIN32) && !defined (__GNUC__)
typedef unsigned __int64 dc_bluetooth_address_t;
#else
typedef unsigned long long dc_bluetooth_address_t;
#endif

/**
 * Convert a bluetooth address to a string.
 *
 * The bluetooth address is formatted as XX:XX:XX:XX:XX:XX, where each
 * XX is a hexadecimal number specifying an octet of the 48-bit address.
 * The minimum size for the buffer is #DC_BLUETOOTH_SIZE bytes.
 *
 * @param[in]  address  A bluetooth address.
 * @param[in]  str      The memory buffer to store the result.
 * @param[in]  size     The size of the memory buffer.
 * @returns The null-terminated string on success, or NULL on failure.
 */
char *
dc_bluetooth_addr2str(dc_bluetooth_address_t address, char *str, size_t size);

/**
 * Convert a string to a bluetooth address.
 *
 * The string is expected to be in the format XX:XX:XX:XX:XX:XX, where
 * each XX is a hexadecimal number specifying an octet of the 48-bit
 * address.
 *
 * @param[in]  address  A null-terminated string.
 * @returns The bluetooth address on success, or zero on failure.
 */
dc_bluetooth_address_t
dc_bluetooth_str2addr(const char *address);

/**
 * Opaque object representing a bluetooth device.
 */
typedef struct dc_bluetooth_device_t dc_bluetooth_device_t;

/**
 * Get the address of the bluetooth device.
 *
 * @param[in]  device  A valid bluetooth device.
 */
dc_bluetooth_address_t
dc_bluetooth_device_get_address (dc_bluetooth_device_t *device);

/**
 * Get the name of the bluetooth device.
 *
 * @param[in]  device  A valid bluetooth device.
 */
const char *
dc_bluetooth_device_get_name (dc_bluetooth_device_t *device);

/**
 * Destroy the bluetooth device and free all resources.
 *
 * @param[in]  device  A valid bluetooth device.
 */
void
dc_bluetooth_device_free (dc_bluetooth_device_t *device);

/**
 * Create an iterator to enumerate the bluetooth devices.
 *
 * @param[out] iterator    A location to store the iterator.
 * @param[in]  context     A valid context object.
 * @param[in]  descriptor  A valid device descriptor or NULL.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_iterator_new (dc_iterator_t **iterator, dc_context_t *context, dc_descriptor_t *descriptor);

/**
 * Open an bluetooth connection.
 *
 * @param[out]  iostream   A location to store the bluetooth connection.
 * @param[in]   context    A valid context object.
 * @param[in]   address    The bluetooth device address.
 * @param[in]   port       The bluetooth port number.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_bluetooth_open (dc_iostream_t **iostream, dc_context_t *context, dc_bluetooth_address_t address, unsigned int port);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BLUETOOTH_H */
