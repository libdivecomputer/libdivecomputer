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

#ifndef DC_DESCRIPTOR_H
#define DC_DESCRIPTOR_H

#include "common.h"
#include "context.h"
#include "iterator.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a supported dive computer.
 */
typedef struct dc_descriptor_t dc_descriptor_t;

/**
 * Create an iterator to enumerate the supported dive computers.
 *
 * @param[out] iterator  A location to store the iterator.
 * @param[in]  context   A valid device descriptor.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_descriptor_iterator_new (dc_iterator_t **iterator, dc_context_t *context);

/* For backwards compatibility */
#define dc_descriptor_iterator(iterator) dc_descriptor_iterator_new(iterator, NULL)

/**
 * Free the device descriptor.
 *
 * @param[in]  descriptor  A valid device descriptor.
 */
void
dc_descriptor_free (dc_descriptor_t *descriptor);

/**
 * Get the vendor name of the dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @returns The vendor name of the dive computer on success, or NULL on failure.
 */
const char *
dc_descriptor_get_vendor (const dc_descriptor_t *descriptor);

/**
 * Get the product name of the dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @returns The product name of the dive computer on success, or NULL on
 * failure.
 */
const char *
dc_descriptor_get_product (const dc_descriptor_t *descriptor);

/**
 * Get the family type of the dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @returns The family type of the dive computer on success, or DC_FAMILY_NULL
 * on failure.
 */
dc_family_t
dc_descriptor_get_type (const dc_descriptor_t *descriptor);

/**
 * Get the model number of the dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @returns The model number of the dive computer on success, or zero on
 * failure.
 */
unsigned int
dc_descriptor_get_model (const dc_descriptor_t *descriptor);

/**
 * Get all transports supported by the dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @returns A bitmask with all the transports supported by the dive computer on
 * success, or DC_TRANSPORT_NONE on failure.
 */
unsigned int
dc_descriptor_get_transports (const dc_descriptor_t *descriptor);

/**
 * Check if a low-level I/O device matches a supported dive computer.
 *
 * @param[in]  descriptor  A valid device descriptor.
 * @param[in]  transport   The transport type of the I/O device.
 * @param[in]  userdata    A pointer to a transport specific data structure:
 *  - DC_TRANSPORT_SERIAL:    Name of the device node (string)
 *  - DC_TRANSPORT_USB:       USB VID/PID (#dc_usb_desc_t)
 *  - DC_TRANSPORT_USBHID:    USB VID/PID (#dc_usbhid_desc_t)
 *  - DC_TRANSPORT_IRDA:      IrDA device name (string)
 *  - DC_TRANSPORT_BLUETOOTH: Bluetooth device name (string)
 *  - DC_TRANSPORT_BLE:       Bluetooth device name (string)
 * @returns Non-zero if the device matches a supported dive computer, or zero if
 * there is no match.
 */
int
dc_descriptor_filter (const dc_descriptor_t *descriptor, dc_transport_t transport, const void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_DESCRIPTOR_H */
