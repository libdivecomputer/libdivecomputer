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

#ifndef DC_RBSTREAM_H
#define DC_RBSTREAM_H

#include <libdivecomputer/device.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a ringbuffer stream.
 */
typedef struct dc_rbstream_t dc_rbstream_t;

/**
 * The ringbuffer read direction.
 */
typedef enum dc_rbstream_direction_t {
	DC_RBSTREAM_FORWARD,
	DC_RBSTREAM_BACKWARD
} dc_rbstream_direction_t;

/**
 * Create a new ringbuffer stream.
 *
 * @param[out]  rbstream    A location to store the ringbuffer stream.
 * @param[in]   device      A valid device object.
 * @param[in]   pagesize    The page size in bytes.
 * @param[in]   packetsize  The packet size in bytes.
 * @param[in]   begin       The ringbuffer begin address.
 * @param[in]   end         The ringbuffer end address.
 * @param[in]   address     The stream start address.
 * @param[in]   direction   The ringbuffer read direction.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_rbstream_new (dc_rbstream_t **rbstream, dc_device_t *device, unsigned int pagesize, unsigned int packetsize, unsigned int begin, unsigned int end, unsigned int address, dc_rbstream_direction_t direction);

/**
 * Read data from the ringbuffer stream.
 *
 * @param[in]  rbstream  A valid ringbuffer stream.
 * @param[in]  progress  An (optional) progress event structure.
 * @param[out] data      The memory buffer to read the data into.
 * @param[in]  size      The number of bytes to read.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_rbstream_read (dc_rbstream_t *rbstream, dc_event_progress_t *progress, unsigned char data[], unsigned int size);

/**
 * Destroy the ringbuffer stream.
 *
 * @param[in]  rbstream  A valid ringbuffer stream.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_rbstream_free (dc_rbstream_t *rbstream);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_RBSTREAM_H */
