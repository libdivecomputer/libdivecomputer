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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBUSB
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#endif
#include <libusb.h>
#endif

#include <libdivecomputer/usb.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "platform.h"
#include "array.h"

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_usb_vtable)

typedef struct dc_usb_params_t {
	unsigned int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
} dc_usb_params_t;

typedef struct dc_usb_config_t {
	dc_usb_desc_t desc;
	dc_usb_params_t params;
} dc_usb_config_t;

typedef struct dc_usb_session_t {
	size_t refcount;
#ifdef HAVE_LIBUSB
	libusb_context *handle;
#endif
} dc_usb_session_t;

struct dc_usb_device_t {
	unsigned short vid, pid;
	dc_usb_session_t *session;
#ifdef HAVE_LIBUSB
	struct libusb_device *handle;
	int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
#endif
};

#ifdef HAVE_LIBUSB
static dc_status_t dc_usb_iterator_next (dc_iterator_t *iterator, void *item);
static dc_status_t dc_usb_iterator_free (dc_iterator_t *iterator);

static dc_status_t dc_usb_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_usb_poll (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_usb_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_usb_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_usb_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);
static dc_status_t dc_usb_close (dc_iostream_t *iostream);

typedef struct dc_usb_iterator_t {
	dc_iterator_t base;
	dc_descriptor_t *descriptor;
	dc_usb_session_t *session;
	struct libusb_device **devices;
	size_t count;
	size_t current;
} dc_usb_iterator_t;

typedef struct dc_usb_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_usb_session_t *session;
	libusb_device_handle *handle;
	int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
	unsigned int timeout;
} dc_usb_t;

static const dc_iterator_vtable_t dc_usb_iterator_vtable = {
	sizeof(dc_usb_iterator_t),
	dc_usb_iterator_next,
	dc_usb_iterator_free,
};

static const dc_iostream_vtable_t dc_usb_vtable = {
	sizeof(dc_usb_t),
	dc_usb_set_timeout, /* set_timeout */
	NULL, /* set_break */
	NULL, /* set_dtr */
	NULL, /* set_rts */
	NULL, /* get_lines */
	NULL, /* get_available */
	NULL, /* configure */
	dc_usb_poll, /* poll */
	dc_usb_read, /* read */
	dc_usb_write, /* write */
	dc_usb_ioctl, /* ioctl */
	NULL, /* flush */
	NULL, /* purge */
	NULL, /* sleep */
	dc_usb_close, /* close */
};

static const dc_usb_config_t g_usb_config[] = {
	// Atomic Aquatics Cobalt
	{{0x0471, 0x0888}, {0, 0x82, 0x02}},
};

static const dc_usb_params_t *
dc_usb_params_find (const dc_usb_desc_t *desc)
{
	if (desc == NULL)
		return NULL;

	for (size_t i = 0; i < C_ARRAY_SIZE(g_usb_config); ++i) {
		if (g_usb_config[i].desc.vid == desc->vid &&
			g_usb_config[i].desc.pid == desc->pid)
			return &g_usb_config[i].params;
	}

	return NULL;
}

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

static dc_status_t
dc_usb_session_new (dc_usb_session_t **out, dc_context_t *context)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_session_t *session = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	session = (dc_usb_session_t *) malloc (sizeof(dc_usb_session_t));
	if (session == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_unlock;
	}

	session->refcount = 1;

	int rc = libusb_init (&session->handle);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to initialize usb support (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_free;
	}

	*out = session;

	return status;

error_free:
	free (session);
error_unlock:
	return status;
}

static dc_usb_session_t *
dc_usb_session_ref (dc_usb_session_t *session)
{
	if (session == NULL)
		return NULL;

	session->refcount++;

	return session;
}

static dc_status_t
dc_usb_session_unref (dc_usb_session_t *session)
{
	if (session == NULL)
		return DC_STATUS_SUCCESS;

	if (--session->refcount == 0) {
		libusb_exit (session->handle);
		free (session);
	}

	return DC_STATUS_SUCCESS;
}
#endif

unsigned int
dc_usb_device_get_vid (dc_usb_device_t *device)
{
	if (device == NULL)
		return 0;

	return device->vid;
}

unsigned int
dc_usb_device_get_pid (dc_usb_device_t *device)
{
	if (device == NULL)
		return 0;

	return device->pid;
}

void
dc_usb_device_free(dc_usb_device_t *device)
{
	if (device == NULL)
		return;

#ifdef HAVE_LIBUSB
	libusb_unref_device (device->handle);
	dc_usb_session_unref (device->session);
#endif
	free (device);
}

dc_status_t
dc_usb_iterator_new (dc_iterator_t **out, dc_context_t *context, dc_descriptor_t *descriptor)
{
#ifdef HAVE_LIBUSB
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_usb_iterator_t *) dc_iterator_allocate (context, &dc_usb_iterator_vtable);
	if (iterator == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the usb library.
	status = dc_usb_session_new (&iterator->session, context);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	// Enumerate the USB devices.
	struct libusb_device **devices = NULL;
	ssize_t ndevices = libusb_get_device_list (iterator->session->handle, &devices);
	if (ndevices < 0) {
		ERROR (context, "Failed to enumerate the usb devices (%s).",
			libusb_error_name (ndevices));
		status = syserror (ndevices);
		goto error_session_unref;
	}

	iterator->devices = devices;
	iterator->count = ndevices;
	iterator->current = 0;
	iterator->descriptor = descriptor;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;

error_session_unref:
	dc_usb_session_unref (iterator->session);
error_free:
	dc_iterator_deallocate ((dc_iterator_t *) iterator);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef HAVE_LIBUSB
static dc_status_t
dc_usb_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_usb_iterator_t *iterator = (dc_usb_iterator_t *) abstract;
	dc_usb_device_t *device = NULL;

	while (iterator->current < iterator->count) {
		struct libusb_device *current = iterator->devices[iterator->current++];

		// Get the device descriptor.
		struct libusb_device_descriptor dev;
		int rc = libusb_get_device_descriptor (current, &dev);
		if (rc < 0) {
			ERROR (abstract->context, "Failed to get the device descriptor (%s).",
				libusb_error_name (rc));
			return syserror (rc);
		}

		dc_usb_desc_t usb = {dev.idVendor, dev.idProduct};
		if (!dc_descriptor_filter (iterator->descriptor, DC_TRANSPORT_USB, &usb)) {
			continue;
		}

		// Check for known USB parameters.
		const dc_usb_params_t *params = dc_usb_params_find (&usb);

		// Get the active configuration descriptor.
		struct libusb_config_descriptor *config = NULL;
		rc = libusb_get_active_config_descriptor (current, &config);
		if (rc != LIBUSB_SUCCESS) {
			ERROR (abstract->context, "Failed to get the configuration descriptor (%s).",
				libusb_error_name (rc));
			return syserror (rc);
		}

		// Find the first matching interface.
		const struct libusb_interface_descriptor *interface = NULL;
		for (unsigned int i = 0; i < config->bNumInterfaces; i++) {
			const struct libusb_interface *iface = &config->interface[i];
			for (int j = 0; j < iface->num_altsetting; j++) {
				const struct libusb_interface_descriptor *desc = &iface->altsetting[j];
				if (interface == NULL &&
					(params == NULL || params->interface == desc->bInterfaceNumber)) {
					interface = desc;
				}
			}
		}

		if (interface == NULL) {
			libusb_free_config_descriptor (config);
			continue;
		}

		// Find the first matching input and output bulk endpoints.
		const struct libusb_endpoint_descriptor *ep_in = NULL, *ep_out = NULL;
		for (unsigned int i = 0; i < interface->bNumEndpoints; i++) {
			const struct libusb_endpoint_descriptor *desc = &interface->endpoint[i];

			unsigned int type = desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
			unsigned int direction = desc->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK;

			if (type != LIBUSB_TRANSFER_TYPE_BULK) {
				continue;
			}

			if (ep_in == NULL && direction == LIBUSB_ENDPOINT_IN &&
				(params == NULL || params->endpoint_in == desc->bEndpointAddress)) {
				ep_in = desc;
			}

			if (ep_out == NULL && direction == LIBUSB_ENDPOINT_OUT &&
				(params == NULL || params->endpoint_out == desc->bEndpointAddress)) {
				ep_out = desc;
			}
		}

		if (ep_in == NULL || ep_out == NULL) {
			libusb_free_config_descriptor (config);
			continue;
		}

		device = (dc_usb_device_t *) malloc (sizeof(dc_usb_device_t));
		if (device == NULL) {
			ERROR (abstract->context, "Failed to allocate memory.");
			libusb_free_config_descriptor (config);
			return DC_STATUS_NOMEMORY;
		}

		device->session = dc_usb_session_ref (iterator->session);
		device->vid = dev.idVendor;
		device->pid = dev.idProduct;
		device->handle = libusb_ref_device (current);
		device->interface = interface->bInterfaceNumber;
		device->endpoint_in = ep_in->bEndpointAddress;
		device->endpoint_out = ep_out->bEndpointAddress;

		*(dc_usb_device_t **) out = device;

		libusb_free_config_descriptor (config);

		return DC_STATUS_SUCCESS;
	}

	return DC_STATUS_DONE;
}

static dc_status_t
dc_usb_iterator_free (dc_iterator_t *abstract)
{
	dc_usb_iterator_t *iterator = (dc_usb_iterator_t *) abstract;

	libusb_free_device_list (iterator->devices, 1);
	dc_usb_session_unref (iterator->session);

	return DC_STATUS_SUCCESS;
}
#endif

dc_status_t
dc_usb_open (dc_iostream_t **out, dc_context_t *context, dc_usb_device_t *device)
{
#ifdef HAVE_LIBUSB
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_t *usb = NULL;

	if (out == NULL || device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: vid=%04x, pid=%04x, interface=%u, endpoints=%02x,%02x",
		device->vid, device->pid, device->interface, device->endpoint_in, device->endpoint_out);

	// Allocate memory.
	usb = (dc_usb_t *) dc_iostream_allocate (context, &dc_usb_vtable, DC_TRANSPORT_USB);
	if (usb == NULL) {
		ERROR (context, "Out of memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the usb library.
	usb->session = dc_usb_session_ref (device->session);
	if (usb->session == NULL) {
		goto error_free;
	}

	// Open the USB device.
	int rc = libusb_open (device->handle, &usb->handle);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to open the usb device (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_session_unref;
	}

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)
	libusb_set_auto_detach_kernel_driver (usb->handle, 1);
#endif

	// Claim the interface.
	rc = libusb_claim_interface (usb->handle, device->interface);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to claim the usb interface (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_usb_close;
	}

	usb->interface = device->interface;
	usb->endpoint_in = device->endpoint_in;
	usb->endpoint_out = device->endpoint_out;
	usb->timeout = 0;

	*out = (dc_iostream_t *) usb;

	return DC_STATUS_SUCCESS;

error_usb_close:
	libusb_close (usb->handle);
error_session_unref:
	dc_usb_session_unref (usb->session);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) usb);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef HAVE_LIBUSB
static dc_status_t
dc_usb_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_t *usb = (dc_usb_t *) abstract;

	libusb_release_interface (usb->handle, usb->interface);
	libusb_close (usb->handle);
	dc_usb_session_unref (usb->session);

	return status;
}

static dc_status_t
dc_usb_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_usb_t *usb = (dc_usb_t *) abstract;

	if (timeout < 0) {
		usb->timeout = 0;
	} else if (timeout == 0) {
		return DC_STATUS_UNSUPPORTED;
	} else {
		usb->timeout = timeout;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_usb_poll (dc_iostream_t *abstract, int timeout)
{
	return DC_STATUS_UNSUPPORTED;
}

static dc_status_t
dc_usb_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_t *usb = (dc_usb_t *) abstract;
	int nbytes = 0;

	int rc = libusb_bulk_transfer (usb->handle, usb->endpoint_in, data, size, &nbytes, usb->timeout);
	if (rc != LIBUSB_SUCCESS || nbytes < 0) {
		ERROR (abstract->context, "Usb read bulk transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		if (nbytes < 0)
			nbytes = 0;
		goto out;
	}

out:
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_usb_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usb_t *usb = (dc_usb_t *) abstract;
	int nbytes = 0;

	int rc = libusb_bulk_transfer (usb->handle, usb->endpoint_out, (void *) data, size, &nbytes, 0);
	if (rc != LIBUSB_SUCCESS || nbytes < 0) {
		ERROR (abstract->context, "Usb write bulk transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		if (nbytes < 0)
			nbytes = 0;
		goto out;
	}

out:
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_usb_ioctl_control (dc_iostream_t *abstract, void *data, size_t size)
{
	dc_usb_t *usb = (dc_usb_t *) abstract;
	const dc_usb_control_t *control = (const dc_usb_control_t *) data;

	if (size < sizeof(control) || control->wLength > size - sizeof(control)) {
		return DC_STATUS_INVALIDARGS;
	}

	int rc = libusb_control_transfer (usb->handle,
		control->bmRequestType, control->bRequest, control->wValue, control->wIndex,
		(unsigned char *) data + sizeof(control), control->wLength, usb->timeout);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (abstract->context, "Usb control transfer failed (%s).",
			libusb_error_name (rc));
		return syserror (rc);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_usb_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	switch (request) {
	case DC_IOCTL_USB_CONTROL_READ:
	case DC_IOCTL_USB_CONTROL_WRITE:
		return dc_usb_ioctl_control (abstract, data, size);
	default:
		return DC_STATUS_UNSUPPORTED;
	}
}
#endif
