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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
#define USBHID
#ifdef _WIN32
#define NOGDI
#endif
#include <libusb-1.0/libusb.h>
#elif defined(HAVE_HIDAPI)
#define USBHID
#include <hidapi/hidapi.h>
#endif

#include "usbhid.h"
#include "common-private.h"
#include "context-private.h"

struct dc_usbhid_t {
	/* Library context. */
	dc_context_t *context;
	/* Internal state. */
#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	libusb_context *ctx;
	libusb_device_handle *handle;
	int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
	unsigned int timeout;
#elif defined(HAVE_HIDAPI)
	hid_device *handle;
	int timeout;
#endif
};

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
static dc_status_t
syserror(int errcode)
{
	switch (errcode) {
	case LIBUSB_ERROR_INVALID_PARAM:
		return DC_STATUS_INVALIDARGS;
	case LIBUSB_ERROR_NO_MEM:
		return DC_STATUS_NOMEMORY;
	case LIBUSB_ERROR_NO_DEVICE:
	case LIBUSB_ERROR_NOT_FOUND:
		return DC_STATUS_NODEVICE;
	case LIBUSB_ERROR_ACCESS:
	case LIBUSB_ERROR_BUSY:
		return DC_STATUS_NOACCESS;
	case LIBUSB_ERROR_TIMEOUT:
		return DC_STATUS_TIMEOUT;
	default:
		return DC_STATUS_IO;
	}
}
#endif

dc_status_t
dc_usbhid_open (dc_usbhid_t **out, dc_context_t *context, unsigned int vid, unsigned int pid)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_t *usbhid = NULL;
	int rc = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: vid=%04x, pid=%04x", vid, pid);

	// Allocate memory.
	usbhid = (dc_usbhid_t *) malloc (sizeof (dc_usbhid_t));
	if (usbhid == NULL) {
		ERROR (context, "Out of memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Library context.
	usbhid->context = context;

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	struct libusb_device **devices = NULL;
	struct libusb_config_descriptor *config = NULL;

	// Initialize the libusb library.
	rc = libusb_init (&usbhid->ctx);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to initialize usb support (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_free;
	}

	// Enumerate the USB devices.
	ssize_t ndevices = libusb_get_device_list (usbhid->ctx, &devices);
	if (ndevices < 0) {
		ERROR (context, "Failed to enumerate the usb devices (%s).",
			libusb_error_name (ndevices));
		status = syserror (ndevices);
		goto error_usb_exit;
	}

	// Find the first device matching the VID/PID.
	struct libusb_device *device = NULL;
	for (size_t i = 0; i < ndevices; i++) {
		struct libusb_device_descriptor desc;
		rc = libusb_get_device_descriptor (devices[i], &desc);
		if (rc < 0) {
			ERROR (context, "Failed to get the device descriptor (%s).",
				libusb_error_name (rc));
			status = syserror (rc);
			goto error_usb_free_list;
		}

		if (desc.idVendor == vid && desc.idProduct == pid) {
			device = devices[i];
			break;
		}
	}

	if (device == NULL) {
		ERROR (context, "No matching USB device found.");
		status = DC_STATUS_NODEVICE;
		goto error_usb_free_list;
	}

	// Get the active configuration descriptor.
	rc = libusb_get_active_config_descriptor (device, &config);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to get the configuration descriptor (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_usb_free_list;
	}

	// Find the first HID interface.
	const struct libusb_interface_descriptor *interface = NULL;
	for (unsigned int i = 0; i < config->bNumInterfaces; i++) {
		const struct libusb_interface *iface = &config->interface[i];
		for (unsigned int j = 0; j < iface->num_altsetting; j++) {
			const struct libusb_interface_descriptor *desc = &iface->altsetting[j];
			if (desc->bInterfaceClass == LIBUSB_CLASS_HID && interface == NULL) {
				interface = desc;
			}
		}
	}

	if (interface == NULL) {
		ERROR (context, "No HID interface found.");
		status = DC_STATUS_IO;
		goto error_usb_free_config;
	}

	// Find the first input and output interrupt endpoints.
	const struct libusb_endpoint_descriptor *ep_in = NULL, *ep_out = NULL;
	for (unsigned int i = 0; i < interface->bNumEndpoints; i++) {
		const struct libusb_endpoint_descriptor *desc = &interface->endpoint[i];

		unsigned int type = desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
		unsigned int direction = desc->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK;

		if (type != LIBUSB_TRANSFER_TYPE_INTERRUPT)
			continue;

		if (direction == LIBUSB_ENDPOINT_IN && ep_in == NULL) {
			ep_in = desc;
		}

		if (direction == LIBUSB_ENDPOINT_OUT && ep_out == NULL) {
			ep_out = desc;
		}
	}

	if (ep_in == NULL || ep_out == NULL) {
		ERROR (context, "No interrupt endpoints found.");
		status = DC_STATUS_IO;
		goto error_usb_free_config;
	}

	usbhid->interface = interface->bInterfaceNumber;
	usbhid->endpoint_in = ep_in->bEndpointAddress;
	usbhid->endpoint_out = ep_out->bEndpointAddress;
	usbhid->timeout = 0;

	INFO (context, "Open: interface=%u, endpoints=%02x,%02x",
		usbhid->interface, usbhid->endpoint_in, usbhid->endpoint_out);

	// Open the USB device.
	rc = libusb_open (device, &usbhid->handle);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to open the usb device (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_usb_free_config;
	}

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)
	libusb_set_auto_detach_kernel_driver (usbhid->handle, 1);
#endif

	// Claim the HID interface.
	rc = libusb_claim_interface (usbhid->handle, usbhid->interface);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to claim the usb interface (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_usb_close;
	}

	libusb_free_config_descriptor (config);
	libusb_free_device_list (devices, 1);

#elif defined(HAVE_HIDAPI)

	// Initialize the hidapi library.
	rc = hid_init();
	if (rc < 0) {
		ERROR (context, "Failed to initialize usb support.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	// Open the USB device.
	usbhid->handle = hid_open (vid, pid, NULL);
	if (usbhid->handle == NULL) {
		ERROR (context, "Failed to open the usb device.");
		status = DC_STATUS_IO;
		goto error_hid_exit;
	}

	usbhid->timeout = -1;
#endif

	*out = usbhid;

	return DC_STATUS_SUCCESS;

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
error_usb_close:
	libusb_close (usbhid->handle);
error_usb_free_config:
	libusb_free_config_descriptor (config);
error_usb_free_list:
	libusb_free_device_list (devices, 1);
error_usb_exit:
	libusb_exit (usbhid->ctx);
#elif defined(HAVE_HIDAPI)
error_hid_exit:
	hid_exit ();
#endif
error_free:
	free (usbhid);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_usbhid_close (dc_usbhid_t *usbhid)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;

	if (usbhid == NULL)
		return DC_STATUS_SUCCESS;

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	libusb_release_interface (usbhid->handle, usbhid->interface);
	libusb_close (usbhid->handle);
	libusb_exit (usbhid->ctx);
#elif defined(HAVE_HIDAPI)
	hid_close(usbhid->handle);
	hid_exit();
#endif
	free (usbhid);

	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_usbhid_set_timeout (dc_usbhid_t *usbhid, int timeout)
{
#ifdef USBHID
	if (usbhid == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (usbhid->context, "Timeout: value=%i", timeout);

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	if (timeout < 0) {
		usbhid->timeout = 0;
	} else if (timeout == 0) {
		return DC_STATUS_UNSUPPORTED;
	} else {
		usbhid->timeout = timeout;
	}
#elif defined(HAVE_HIDAPI)
	if (timeout < 0) {
		usbhid->timeout = -1;
	} else {
		usbhid->timeout = timeout;
	}
#endif

	return DC_STATUS_SUCCESS;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_usbhid_read (dc_usbhid_t *usbhid, void *data, size_t size, size_t *actual)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;
	int nbytes = 0;

	if (usbhid == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out_invalidargs;
	}

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	int rc = libusb_interrupt_transfer (usbhid->handle, usbhid->endpoint_in, data, size, &nbytes, usbhid->timeout);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (usbhid->context, "Usb read interrupt transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto out;
	}
#elif defined(HAVE_HIDAPI)
	nbytes = hid_read_timeout(usbhid->handle, data, size, usbhid->timeout);
	if (nbytes < 0) {
		ERROR (usbhid->context, "Usb read interrupt transfer failed.");
		status = DC_STATUS_IO;
		goto out;
	}
#endif

out:
	HEXDUMP (usbhid->context, DC_LOGLEVEL_INFO, "Read", (unsigned char *) data, nbytes);

out_invalidargs:
	if (actual)
		*actual = nbytes;

	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_usbhid_write (dc_usbhid_t *usbhid, const void *data, size_t size, size_t *actual)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;
	int nbytes = 0;

	if (usbhid == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out_invalidargs;
	}

#if defined(HAVE_LIBUSB) && !defined(__APPLE__)
	int rc = libusb_interrupt_transfer (usbhid->handle, usbhid->endpoint_out, (void *) data, size, &nbytes, 0);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (usbhid->context, "Usb write interrupt transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto out;
	}
#elif defined(HAVE_HIDAPI)
	nbytes = hid_write(usbhid->handle, data, size);
	if (nbytes < 0) {
		ERROR (usbhid->context, "Usb write interrupt transfer failed.");
		status = DC_STATUS_IO;
		goto out;
	}
#endif

out:
	HEXDUMP (usbhid->context, DC_LOGLEVEL_INFO, "Write", (unsigned char *) data, nbytes);

out_invalidargs:
	if (actual)
		*actual = nbytes;

	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}
