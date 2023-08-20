/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Jef Driesen
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

#ifndef DC_USB_H
#define DC_USB_H

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
 * Perform a USB control transfer.
 *
 * The parameters for the control transfer are specified in the
 * #dc_usb_control_t data structure. If the control transfer requires
 * additional data as in- or output, the buffer must be located
 * immediately after the #dc_usb_control_t data structure, and the
 * length of the buffer must be indicated in the #wLength field. The
 * size of the ioctl request is the total size, including the size of
 * the #dc_usb_control_t structure.
 */
#define DC_IOCTL_USB_CONTROL_READ  DC_IOCTL_IOR('u', 0, DC_IOCTL_SIZE_VARIABLE)
#define DC_IOCTL_USB_CONTROL_WRITE DC_IOCTL_IOW('u', 0, DC_IOCTL_SIZE_VARIABLE)

/**
 * USB control transfer.
 */
typedef struct dc_usb_control_t {
	unsigned char  bmRequestType;
	unsigned char  bRequest;
	unsigned short wValue;
	unsigned short wIndex;
	unsigned short wLength;
} dc_usb_control_t;

/**
 * Endpoint direction bits of the USB control transfer.
 */
typedef enum dc_usb_endpoint_t {
	DC_USB_ENDPOINT_OUT = 0x00,
	DC_USB_ENDPOINT_IN = 0x80
} dc_usb_endpoint_t;

/**
 * Request type bits of the USB control transfer.
 */
typedef enum dc_usb_request_t {
	DC_USB_REQUEST_STANDARD = 0x00,
	DC_USB_REQUEST_CLASS = 0x20,
	DC_USB_REQUEST_VENDOR = 0x40,
	DC_USB_REQUEST_RESERVED = 0x60
} dc_usb_request_t;

/**
 * Recipient bits of the USB control transfer.
 */
typedef enum dc_usb_recipient_t {
	DC_USB_RECIPIENT_DEVICE = 0x00,
	DC_USB_RECIPIENT_INTERFACE = 0x01,
	DC_USB_RECIPIENT_ENDPOINT = 0x02,
	DC_USB_RECIPIENT_OTHER = 0x03,
} dc_usb_recipient_t;

/**
 * USB device descriptor.
 */
typedef struct dc_usb_desc_t {
	unsigned short vid;
	unsigned short pid;
} dc_usb_desc_t;

/**
 * Opaque object representing a USB device.
 */
typedef struct dc_usb_device_t dc_usb_device_t;

/**
 * Get the vendor id (VID) of the USB device.
 *
 * @param[in]  device  A valid USB device.
 */
unsigned int
dc_usb_device_get_vid (dc_usb_device_t *device);

/**
 * Get the product id (PID) of the USB device.
 *
 * @param[in]  device  A valid USB device.
 */
unsigned int
dc_usb_device_get_pid (dc_usb_device_t *device);

/**
 * Destroy the USB device and free all resources.
 *
 * @param[in]  device  A valid USB device.
 */
void
dc_usb_device_free(dc_usb_device_t *device);

/**
 * Create an iterator to enumerate the USB devices.
 *
 * @param[out] iterator    A location to store the iterator.
 * @param[in]  context     A valid context object.
 * @param[in]  descriptor  A valid device descriptor or NULL.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usb_iterator_new (dc_iterator_t **iterator, dc_context_t *context, dc_descriptor_t *descriptor);

/**
 * Open a USB connection.
 *
 * @param[out]  iostream A location to store the USB connection.
 * @param[in]   context  A valid context object.
 * @param[in]   device   A valid USB device.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_usb_open (dc_iostream_t **iostream, dc_context_t *context, dc_usb_device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_USB_H */
