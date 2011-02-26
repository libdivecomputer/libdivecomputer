/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#ifdef HAVE_LIBUSB
#include <libusb-1.0/libusb.h>
#endif

#include "device-private.h"
#include "atomics_cobalt.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define VID 0x0471
#define PID 0x0888
#define TIMEOUT 1000

#define FP_OFFSET 20

typedef struct atomics_cobalt_device_t {
	device_t base;
#ifdef HAVE_LIBUSB
	libusb_context *context;
	libusb_device_handle *handle;
#endif
	unsigned char fingerprint[6];
} atomics_cobalt_device_t;

static device_status_t atomics_cobalt_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t atomics_cobalt_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t atomics_cobalt_device_close (device_t *abstract);

static const device_backend_t atomics_cobalt_device_backend = {
	DEVICE_TYPE_ATOMICS_COBALT,
	atomics_cobalt_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	atomics_cobalt_device_foreach, /* foreach */
	atomics_cobalt_device_close /* close */
};

static int
device_is_atomics_cobalt (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &atomics_cobalt_device_backend;
}


device_status_t
atomics_cobalt_device_open (device_t **out)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

#ifdef HAVE_LIBUSB
	// Allocate memory.
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) malloc (sizeof (atomics_cobalt_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &atomics_cobalt_device_backend);

	// Set the default values.
	device->context = NULL;
	device->handle = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	int rc = libusb_init (&device->context);
	if (rc < 0) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	device->handle = libusb_open_device_with_vid_pid (device->context, VID, PID);
	if (device->handle == NULL) {
		libusb_exit (device->context);
		free (device);
		return DEVICE_STATUS_IO;
	}

	rc = libusb_claim_interface (device->handle, 0);
	if (rc < 0) {
		libusb_close (device->handle);
		libusb_exit (device->context);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
#else
	return DEVICE_STATUS_UNSUPPORTED;
#endif
}


static device_status_t
atomics_cobalt_device_close (device_t *abstract)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

#ifdef HAVE_LIBUSB
	libusb_release_interface(device->handle, 0);
	libusb_close (device->handle);
	libusb_exit (device->context);
#endif

	// Free memory.
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_read_dive (device_t *abstract, dc_buffer_t *buffer, int init)
{
#ifdef HAVE_LIBUSB
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DEVICE_STATUS_CANCELLED;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Send the command to the dive computer.
	uint8_t bRequest = init ? 0x09 : 0x0A;
	int rc = libusb_control_transfer (device->handle,
		LIBUSB_RECIPIENT_DEVICE | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		bRequest, 0, 0, NULL, 0, TIMEOUT);
	if (rc != LIBUSB_SUCCESS)
		return DEVICE_STATUS_IO;

	unsigned int nbytes = 0;
	while (1) {
		// Receive the answer from the dive computer.
		int length = 0;
		unsigned char packet[10 * 1024] = {0};
		rc = libusb_bulk_transfer (device->handle, 0x82,
			packet, sizeof (packet), &length, TIMEOUT);
		if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_TIMEOUT)
			return DEVICE_STATUS_IO;

		// Append the packet to the output buffer.
		dc_buffer_append (buffer, packet, length);
		nbytes += length;

		// If we received fewer bytes than requested, the transfer is finished.
		if (length < sizeof (packet))
			break;
	}

	// Check for a buffer error.
	if (dc_buffer_get_size (buffer) != nbytes) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Check for the minimum length.
	if (nbytes < 2) {
		WARNING ("Data packet is too short.");
		return DEVICE_STATUS_ERROR;
	}

#if 0
	// Verify the checksum of the packet.
	unsigned char *data = dc_buffer_get_data (buffer);
	unsigned short crc = array_uint16_le (data + nbytes - 2);
	unsigned short ccrc = checksum_add_uint16 (data, nbytes - 2, 0x0);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}
#endif

	// Remove the checksum bytes.
	dc_buffer_slice (buffer, 0, nbytes - 2);

	return DEVICE_STATUS_SUCCESS;
#else
	return DEVICE_STATUS_UNSUPPORTED;
#endif
}


static device_status_t
atomics_cobalt_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	unsigned int ndives = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = atomics_cobalt_read_dive (abstract, buffer, (ndives == 0))) == DEVICE_STATUS_SUCCESS) {
		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);

		if (size == 0) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (memcmp (data + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (callback && !callback (data, size, data + FP_OFFSET, sizeof (device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		ndives++;
	}

	dc_buffer_free (buffer);

	return rc;
}
