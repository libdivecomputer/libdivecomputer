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
#include <string.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#if defined(HAVE_HIDAPI)
#define USE_HIDAPI
#define USBHID
#elif defined(HAVE_LIBUSB) && !defined(__APPLE__)
#define USE_LIBUSB
#define USBHID
#endif

#if defined(USE_LIBUSB)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#endif
#include <libusb.h>
#elif defined(USE_HIDAPI)
#include <hidapi.h>
#endif

#include <libdivecomputer/usbhid.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "platform.h"

#ifdef _WIN32
typedef LONG dc_mutex_t;
#define DC_MUTEX_INIT 0
#else
typedef pthread_mutex_t dc_mutex_t;
#define DC_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#endif

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_usbhid_vtable)

typedef struct dc_usbhid_session_t {
	size_t refcount;
#if defined(USE_LIBUSB)
	libusb_context *handle;
#endif
} dc_usbhid_session_t;

struct dc_usbhid_device_t {
	unsigned short vid, pid;
	dc_usbhid_session_t *session;
#if defined(USE_LIBUSB)
	struct libusb_device *handle;
	int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
	unsigned short packetsize;
#elif defined(USE_HIDAPI)
	char *path;
#endif
};

#ifdef USBHID
static dc_status_t dc_usbhid_iterator_next (dc_iterator_t *iterator, void *item);
static dc_status_t dc_usbhid_iterator_free (dc_iterator_t *iterator);

static dc_status_t dc_usbhid_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_usbhid_poll (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_usbhid_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_usbhid_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_usbhid_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);
static dc_status_t dc_usbhid_close (dc_iostream_t *iostream);

typedef struct dc_usbhid_iterator_t {
	dc_iterator_t base;
	dc_descriptor_t *descriptor;
	dc_usbhid_session_t *session;
#if defined(USE_LIBUSB)
	struct libusb_device **devices;
	size_t count;
	size_t current;
#elif defined(USE_HIDAPI)
	struct hid_device_info *devices, *current;
#endif
} dc_usbhid_iterator_t;

typedef struct dc_usbhid_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_usbhid_session_t *session;
#if defined(USE_LIBUSB)
	libusb_device_handle *handle;
	int interface;
	unsigned char endpoint_in;
	unsigned char endpoint_out;
	unsigned short packetsize;
	unsigned int timeout;
#elif defined(USE_HIDAPI)
	hid_device *handle;
	int timeout;
#endif
} dc_usbhid_t;

static const dc_iterator_vtable_t dc_usbhid_iterator_vtable = {
	sizeof(dc_usbhid_iterator_t),
	dc_usbhid_iterator_next,
	dc_usbhid_iterator_free,
};

static const dc_iostream_vtable_t dc_usbhid_vtable = {
	sizeof(dc_usbhid_t),
	dc_usbhid_set_timeout, /* set_timeout */
	NULL, /* set_break */
	NULL, /* set_dtr */
	NULL, /* set_rts */
	NULL, /* get_lines */
	NULL, /* get_available */
	NULL, /* configure */
	dc_usbhid_poll, /* poll */
	dc_usbhid_read, /* read */
	dc_usbhid_write, /* write */
	dc_usbhid_ioctl, /* ioctl */
	NULL, /* flush */
	NULL, /* purge */
	NULL, /* sleep */
	dc_usbhid_close, /* close */
};

#ifdef USE_HIDAPI
static dc_mutex_t g_usbhid_mutex = DC_MUTEX_INIT;
static dc_usbhid_session_t *g_usbhid_session = NULL;
#endif

#if defined(USE_LIBUSB)
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

#ifdef USE_HIDAPI
static void
dc_mutex_lock (dc_mutex_t *mutex)
{
#ifdef _WIN32
	while (InterlockedCompareExchange (mutex, 1, 0) == 1) {
		SleepEx (0, TRUE);
	}
#else
	pthread_mutex_lock (mutex);
#endif
}

static void
dc_mutex_unlock (dc_mutex_t *mutex)
{
#ifdef _WIN32
	InterlockedExchange (mutex, 0);
#else
	pthread_mutex_unlock (mutex);
#endif
}
#endif

static dc_status_t
dc_usbhid_session_new (dc_usbhid_session_t **out, dc_context_t *context)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_session_t *session = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef USE_HIDAPI
	dc_mutex_lock (&g_usbhid_mutex);

	if (g_usbhid_session) {
		g_usbhid_session->refcount++;
		*out = g_usbhid_session;
		dc_mutex_unlock (&g_usbhid_mutex);
		return DC_STATUS_SUCCESS;
	}
#endif

	session = (dc_usbhid_session_t *) malloc (sizeof(dc_usbhid_session_t));
	if (session == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_unlock;
	}

	session->refcount = 1;

#if defined(USE_LIBUSB)
	int rc = libusb_init (&session->handle);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to initialize usb support (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_free;
	}
#elif defined(USE_HIDAPI)
	int rc = hid_init();
	if (rc < 0) {
		ERROR (context, "Failed to initialize usb support.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	g_usbhid_session = session;

	dc_mutex_unlock (&g_usbhid_mutex);
#endif

	*out = session;

	return status;

error_free:
	free (session);
error_unlock:
#ifdef USE_HIDAPI
	dc_mutex_unlock (&g_usbhid_mutex);
#endif
	return status;
}

static dc_usbhid_session_t *
dc_usbhid_session_ref (dc_usbhid_session_t *session)
{
	if (session == NULL)
		return NULL;

#ifdef USE_HIDAPI
	dc_mutex_lock (&g_usbhid_mutex);
#endif

	session->refcount++;

#ifdef USE_HIDAPI
	dc_mutex_unlock (&g_usbhid_mutex);
#endif

	return session;
}

static dc_status_t
dc_usbhid_session_unref (dc_usbhid_session_t *session)
{
	if (session == NULL)
		return DC_STATUS_SUCCESS;

#ifdef USE_HIDAPI
	dc_mutex_lock (&g_usbhid_mutex);
#endif

	if (--session->refcount == 0) {
#if defined(USE_LIBUSB)
		libusb_exit (session->handle);
#elif defined(USE_HIDAPI)
		hid_exit ();
		g_usbhid_session = NULL;
#endif
		free (session);
	}

#ifdef USE_HIDAPI
	dc_mutex_unlock (&g_usbhid_mutex);
#endif

	return DC_STATUS_SUCCESS;
}
#endif

unsigned int
dc_usbhid_device_get_vid (dc_usbhid_device_t *device)
{
	if (device == NULL)
		return 0;

	return device->vid;
}

unsigned int
dc_usbhid_device_get_pid (dc_usbhid_device_t *device)
{
	if (device == NULL)
		return 0;

	return device->pid;
}

void
dc_usbhid_device_free(dc_usbhid_device_t *device)
{
	if (device == NULL)
		return;

#ifdef USBHID
#if defined(USE_LIBUSB)
	libusb_unref_device (device->handle);
#elif defined(USE_HIDAPI)
	free (device->path);
#endif
	dc_usbhid_session_unref (device->session);
#endif
	free (device);
}

dc_status_t
dc_usbhid_iterator_new (dc_iterator_t **out, dc_context_t *context, dc_descriptor_t *descriptor)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_usbhid_iterator_t *) dc_iterator_allocate (context, &dc_usbhid_iterator_vtable);
	if (iterator == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the usb library.
	status = dc_usbhid_session_new (&iterator->session, context);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

#if defined(USE_LIBUSB)
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
#elif defined(USE_HIDAPI)
	struct hid_device_info *devices = hid_enumerate(0x0, 0x0);
	if (devices == NULL) {
		status = DC_STATUS_IO;
		goto error_session_unref;
	}

	iterator->devices = devices;
	iterator->current = devices;
#endif
	iterator->descriptor = descriptor;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;

error_session_unref:
	dc_usbhid_session_unref (iterator->session);
error_free:
	dc_iterator_deallocate ((dc_iterator_t *) iterator);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef USBHID
static dc_status_t
dc_usbhid_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_usbhid_iterator_t *iterator = (dc_usbhid_iterator_t *) abstract;
	dc_usbhid_device_t *device = NULL;

#if defined(USE_LIBUSB)
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

		dc_usbhid_desc_t usb = {dev.idVendor, dev.idProduct};
		if (!dc_descriptor_filter (iterator->descriptor, DC_TRANSPORT_USBHID, &usb)) {
			continue;
		}

		// Get the active configuration descriptor.
		struct libusb_config_descriptor *config = NULL;
		rc = libusb_get_active_config_descriptor (current, &config);
		if (rc != LIBUSB_SUCCESS) {
			ERROR (abstract->context, "Failed to get the configuration descriptor (%s).",
				libusb_error_name (rc));
			return syserror (rc);
		}

		// Find the first HID interface.
		const struct libusb_interface_descriptor *interface = NULL;
		for (unsigned int i = 0; i < config->bNumInterfaces; i++) {
			const struct libusb_interface *iface = &config->interface[i];
			for (int j = 0; j < iface->num_altsetting; j++) {
				const struct libusb_interface_descriptor *desc = &iface->altsetting[j];
				if (desc->bInterfaceClass == LIBUSB_CLASS_HID && interface == NULL) {
					interface = desc;
				}
			}
		}

		if (interface == NULL) {
			libusb_free_config_descriptor (config);
			continue;
		}

		// Find the first input and output interrupt endpoints.
		const struct libusb_endpoint_descriptor *ep_in = NULL, *ep_out = NULL;
		for (unsigned int i = 0; i < interface->bNumEndpoints; i++) {
			const struct libusb_endpoint_descriptor *desc = &interface->endpoint[i];

			unsigned int type = desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
			unsigned int direction = desc->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK;

			if (type != LIBUSB_TRANSFER_TYPE_INTERRUPT) {
				continue;
			}

			if (direction == LIBUSB_ENDPOINT_IN && ep_in == NULL) {
				ep_in = desc;
			}

			if (direction == LIBUSB_ENDPOINT_OUT && ep_out == NULL) {
				ep_out = desc;
			}
		}

		if (ep_in == NULL || ep_out == NULL) {
			libusb_free_config_descriptor (config);
			continue;
		}

		device = (dc_usbhid_device_t *) malloc (sizeof(dc_usbhid_device_t));
		if (device == NULL) {
			ERROR (abstract->context, "Failed to allocate memory.");
			libusb_free_config_descriptor (config);
			return DC_STATUS_NOMEMORY;
		}

		device->session = dc_usbhid_session_ref (iterator->session);
		device->vid = dev.idVendor;
		device->pid = dev.idProduct;
		device->handle = libusb_ref_device (current);
		device->interface = interface->bInterfaceNumber;
		device->endpoint_in = ep_in->bEndpointAddress;
		device->endpoint_out = ep_out->bEndpointAddress;
		device->packetsize = ep_in->wMaxPacketSize;

		*(dc_usbhid_device_t **) out = device;

		libusb_free_config_descriptor (config);

		return DC_STATUS_SUCCESS;
	}
#elif defined(USE_HIDAPI)
	while (iterator->current) {
		struct hid_device_info *current = iterator->current;
		iterator->current = current->next;

		dc_usbhid_desc_t usb = {current->vendor_id, current->product_id};
		if (!dc_descriptor_filter (iterator->descriptor, DC_TRANSPORT_USBHID, &usb)) {
			continue;
		}

		device = (dc_usbhid_device_t *) malloc (sizeof(dc_usbhid_device_t));
		if (device == NULL) {
			ERROR (abstract->context, "Failed to allocate memory.");
			return DC_STATUS_NOMEMORY;
		}

		device->session = dc_usbhid_session_ref (iterator->session);
		device->vid = current->vendor_id;
		device->pid = current->product_id;
		device->path = strdup (current->path);

		*(dc_usbhid_device_t **) out = device;

		return DC_STATUS_SUCCESS;
	}
#endif

	return DC_STATUS_DONE;
}

static dc_status_t
dc_usbhid_iterator_free (dc_iterator_t *abstract)
{
	dc_usbhid_iterator_t *iterator = (dc_usbhid_iterator_t *) abstract;

#if defined(USE_LIBUSB)
	libusb_free_device_list (iterator->devices, 1);
#elif defined(USE_HIDAPI)
	hid_free_enumeration (iterator->devices);
#endif
	dc_usbhid_session_unref (iterator->session);

	return DC_STATUS_SUCCESS;
}
#endif

dc_status_t
dc_usbhid_open (dc_iostream_t **out, dc_context_t *context, dc_usbhid_device_t *device)
{
#ifdef USBHID
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_t *usbhid = NULL;

	if (out == NULL || device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: vid=%04x, pid=%04x", device->vid, device->pid);

	// Allocate memory.
	usbhid = (dc_usbhid_t *) dc_iostream_allocate (context, &dc_usbhid_vtable, DC_TRANSPORT_USBHID);
	if (usbhid == NULL) {
		ERROR (context, "Out of memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the usb library.
	usbhid->session = dc_usbhid_session_ref (device->session);
	if (usbhid->session == NULL) {
		goto error_free;
	}

#if defined(USE_LIBUSB)
	INFO (context, "Open: interface=%u, endpoints=%02x,%02x",
		device->interface, device->endpoint_in, device->endpoint_out);

	// Open the USB device.
	int rc = libusb_open (device->handle, &usbhid->handle);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to open the usb device (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_session_unref;
	}

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)
	libusb_set_auto_detach_kernel_driver (usbhid->handle, 1);
#endif

	// Claim the HID interface.
	rc = libusb_claim_interface (usbhid->handle, device->interface);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (context, "Failed to claim the usb interface (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		goto error_usb_close;
	}

	usbhid->interface = device->interface;
	usbhid->endpoint_in = device->endpoint_in;
	usbhid->endpoint_out = device->endpoint_out;
	usbhid->packetsize = device->packetsize;
	usbhid->timeout = 0;

#elif defined(USE_HIDAPI)
	INFO (context, "Open: path=%s", device->path);

	// Open the USB device.
	usbhid->handle = hid_open_path (device->path);
	if (usbhid->handle == NULL) {
		ERROR (context, "Failed to open the usb device.");
		status = DC_STATUS_IO;
		goto error_session_unref;
	}

	usbhid->timeout = -1;
#endif

	*out = (dc_iostream_t *) usbhid;

	return DC_STATUS_SUCCESS;

#if defined(USE_LIBUSB)
error_usb_close:
	libusb_close (usbhid->handle);
#endif
error_session_unref:
	dc_usbhid_session_unref (usbhid->session);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) usbhid);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef USBHID
static dc_status_t
dc_usbhid_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_t *usbhid = (dc_usbhid_t *) abstract;

#if defined(USE_LIBUSB)
	libusb_release_interface (usbhid->handle, usbhid->interface);
	libusb_close (usbhid->handle);
#elif defined(USE_HIDAPI)
	hid_close(usbhid->handle);
#endif
	dc_usbhid_session_unref (usbhid->session);

	return status;
}

static dc_status_t
dc_usbhid_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_usbhid_t *usbhid = (dc_usbhid_t *) abstract;

#if defined(USE_LIBUSB)
	if (timeout < 0) {
		usbhid->timeout = 0;
	} else if (timeout == 0) {
		return DC_STATUS_UNSUPPORTED;
	} else {
		usbhid->timeout = timeout;
	}
#elif defined(USE_HIDAPI)
	if (timeout < 0) {
		usbhid->timeout = -1;
	} else {
		usbhid->timeout = timeout;
	}
#endif

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_usbhid_poll (dc_iostream_t *abstract, int timeout)
{
	return DC_STATUS_UNSUPPORTED;
}

static dc_status_t
dc_usbhid_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_t *usbhid = (dc_usbhid_t *) abstract;
	int nbytes = 0;

#if defined(USE_LIBUSB)
	if (size > usbhid->packetsize) {
		size = usbhid->packetsize;
	}

	int rc = libusb_interrupt_transfer (usbhid->handle, usbhid->endpoint_in, data, size, &nbytes, usbhid->timeout);
	if (rc != LIBUSB_SUCCESS || nbytes < 0) {
		ERROR (abstract->context, "Usb read interrupt transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		if (nbytes < 0)
			nbytes = 0;
		goto out;
	}
#elif defined(USE_HIDAPI)
	nbytes = hid_read_timeout(usbhid->handle, data, size, usbhid->timeout);
	if (nbytes < 0) {
		ERROR (abstract->context, "Usb read interrupt transfer failed.");
		status = DC_STATUS_IO;
		nbytes = 0;
		goto out;
	}
#endif

out:
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_usbhid_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_usbhid_t *usbhid = (dc_usbhid_t *) abstract;
	int nbytes = 0;

	if (size == 0) {
		goto out;
	}

#if defined(USE_LIBUSB)
	const unsigned char *buffer = (const unsigned char *) data;
	size_t length = size;

	// Skip a report id of zero.
	unsigned char report = buffer[0];
	if (report == 0) {
		buffer++;
		length--;
	}

	int rc = libusb_interrupt_transfer (usbhid->handle, usbhid->endpoint_out, (void *) buffer, length, &nbytes, 0);
	if (rc != LIBUSB_SUCCESS || nbytes < 0) {
		ERROR (abstract->context, "Usb write interrupt transfer failed (%s).",
			libusb_error_name (rc));
		status = syserror (rc);
		if (nbytes < 0)
			nbytes = 0;
		goto out;
	}

	if (report == 0) {
		nbytes++;
	}
#elif defined(USE_HIDAPI)
	nbytes = hid_write(usbhid->handle, data, size);
	if (nbytes < 0) {
		ERROR (abstract->context, "Usb write interrupt transfer failed.");
		status = DC_STATUS_IO;
		nbytes = 0;
		goto out;
	}
#endif

out:
#ifdef _WIN32
	if ((size_t) nbytes > size) {
		WARNING (abstract->context, "Number of bytes exceeds the buffer size (" DC_PRINTF_SIZE " > " DC_PRINTF_SIZE ")!", nbytes, size);
		nbytes = size;
	}
#endif

	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_usbhid_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	return DC_STATUS_UNSUPPORTED;
}
#endif
