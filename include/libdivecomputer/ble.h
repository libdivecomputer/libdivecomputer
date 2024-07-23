/*
 * libdivecomputer
 *
 * Copyright (C) 2019 Jef Driesen
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

#ifndef DC_BLE_H
#define DC_BLE_H

#include "ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Get the remote device name.
 */
#define DC_IOCTL_BLE_GET_NAME   DC_IOCTL_IOR('b', 0, DC_IOCTL_SIZE_VARIABLE)

/**
 * Get the bluetooth authentication PIN code.
 *
 * The data format is a NULL terminated string.
 */
#define DC_IOCTL_BLE_GET_PINCODE   DC_IOCTL_IOR('b', 1, DC_IOCTL_SIZE_VARIABLE)

/**
 * Get/set the bluetooth authentication access code.
 *
 * The data format is a variable sized byte array.
 */
#define DC_IOCTL_BLE_GET_ACCESSCODE   DC_IOCTL_IOR('b', 2, DC_IOCTL_SIZE_VARIABLE)
#define DC_IOCTL_BLE_SET_ACCESSCODE   DC_IOCTL_IOW('b', 2, DC_IOCTL_SIZE_VARIABLE)

/**
 * Perform a BLE characteristic read/write operation.
 *
 * The UUID of the characteristic must be specified as a #dc_ble_uuid_t
 * data structure. If the operation requires additional data as in- or
 * output, the buffer must be located immediately after the
 * #dc_ble_uuid_t data structure. The size of the ioctl request is the
 * total size, including the size of the #dc_ble_uuid_t structure.
 */
#define DC_IOCTL_BLE_CHARACTERISTIC_READ  DC_IOCTL_IOR('b', 3, DC_IOCTL_SIZE_VARIABLE)
#define DC_IOCTL_BLE_CHARACTERISTIC_WRITE DC_IOCTL_IOW('b', 3, DC_IOCTL_SIZE_VARIABLE)

/**
 * The minimum number of bytes (including the terminating null byte) for
 * formatting a bluetooth UUID as a string.
 */
#define DC_BLE_UUID_SIZE 37

/**
 * Bluetooth UUID (128 bits).
 */
typedef unsigned char dc_ble_uuid_t[16];

/**
 * Convert a bluetooth UUID to a string.
 *
 * The bluetooth UUID is formatted as
 * XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX, where each XX pair is a
 * hexadecimal number specifying an octet of the UUID.
 * The minimum size for the buffer is #DC_BLE_UUID_SIZE bytes.
 *
 * @param[in]  uuid     A bluetooth UUID.
 * @param[in]  str      The memory buffer to store the result.
 * @param[in]  size     The size of the memory buffer.
 * @returns The null-terminated string on success, or NULL on failure.
 */
char *
dc_ble_uuid2str (const dc_ble_uuid_t uuid, char *str, size_t size);

/**
 * Convert a string to a bluetooth UUID.
 *
 * The string is expected to be in the format
 * XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX, where each XX pair is a
 * hexadecimal number specifying an octet of the UUID.
 *
 * @param[in]  str      A null-terminated string.
 * @param[in]  uuid     The memory buffer to store the result.
 * @returns Non-zero on success, or zero on failure.
 */
int
dc_ble_str2uuid (const char *str, dc_ble_uuid_t uuid);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BLE_H */
