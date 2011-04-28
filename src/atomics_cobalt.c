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

#define SZ_MEMORY (29 * 64 * 1024)
#define SZ_VERSION 14

typedef struct atomics_cobalt_device_t {
	device_t base;
#ifdef HAVE_LIBUSB
	libusb_context *context;
	libusb_device_handle *handle;
#endif
	unsigned int simulation;
	unsigned char fingerprint[6];
	unsigned char version[SZ_VERSION];
} atomics_cobalt_device_t;

static device_status_t atomics_cobalt_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t atomics_cobalt_device_version (device_t *abstract, unsigned char data[], unsigned int size);
static device_status_t atomics_cobalt_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t atomics_cobalt_device_close (device_t *abstract);

static const device_backend_t atomics_cobalt_device_backend = {
	DEVICE_TYPE_ATOMICS_COBALT,
	atomics_cobalt_device_set_fingerprint, /* set_fingerprint */
	atomics_cobalt_device_version, /* version */
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
	device->simulation = 0;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	int rc = libusb_init (&device->context);
	if (rc < 0) {
		WARNING ("Failed to initialize usb support.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	device->handle = libusb_open_device_with_vid_pid (device->context, VID, PID);
	if (device->handle == NULL) {
		WARNING ("Failed to open the usb device.");
		libusb_exit (device->context);
		free (device);
		return DEVICE_STATUS_IO;
	}

	rc = libusb_claim_interface (device->handle, 0);
	if (rc < 0) {
		WARNING ("Failed to claim the usb interface.");
		libusb_close (device->handle);
		libusb_exit (device->context);
		free (device);
		return DEVICE_STATUS_IO;
	}

	device_status_t status = atomics_cobalt_device_version ((device_t *) device, device->version, sizeof (device->version));
	if (status != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to identify the dive computer.");
		libusb_close (device->handle);
		libusb_exit (device->context);
		free (device);
		return status;
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


device_status_t
atomics_cobalt_device_set_simulation (device_t *abstract, unsigned int simulation)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->simulation = simulation;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (size < SZ_VERSION)
		return DEVICE_STATUS_MEMORY;

#ifdef HAVE_LIBUSB
	// Send the command to the dive computer.
	uint8_t bRequest = 0x01;
	int rc = libusb_control_transfer (device->handle,
		LIBUSB_RECIPIENT_DEVICE | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		bRequest, 0, 0, NULL, 0, TIMEOUT);
	if (rc != LIBUSB_SUCCESS) {
		WARNING ("Failed to send the command.");
		return DEVICE_STATUS_IO;
	}

	// Receive the answer from the dive computer.
	int length = 0;
	unsigned char packet[SZ_VERSION + 2] = {0};
	rc = libusb_bulk_transfer (device->handle, 0x82,
		packet, sizeof (packet), &length, TIMEOUT);
	if (rc != LIBUSB_SUCCESS || length != sizeof (packet)) {
		WARNING ("Failed to receive the answer.");
		return DEVICE_STATUS_IO;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (packet + SZ_VERSION);
	unsigned short ccrc = checksum_add_uint16 (packet, SZ_VERSION, 0x0);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	memcpy (data, packet, SZ_VERSION);

	return DEVICE_STATUS_SUCCESS;
#else
	return DEVICE_STATUS_UNSUPPORTED;
#endif
}


static device_status_t
atomics_cobalt_read_dive (device_t *abstract, dc_buffer_t *buffer, int init, device_progress_t *progress)
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
	uint8_t bRequest = 0;
	if (device->simulation)
		bRequest = init ? 0x02 : 0x03;
	else
		bRequest = init ? 0x09 : 0x0A;
	int rc = libusb_control_transfer (device->handle,
		LIBUSB_RECIPIENT_DEVICE | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		bRequest, 0, 0, NULL, 0, TIMEOUT);
	if (rc != LIBUSB_SUCCESS) {
		WARNING ("Failed to send the command.");
		return DEVICE_STATUS_IO;
	}

	unsigned int nbytes = 0;
	while (1) {
		// Receive the answer from the dive computer.
		int length = 0;
		unsigned char packet[10 * 1024] = {0};
		rc = libusb_bulk_transfer (device->handle, 0x82,
			packet, sizeof (packet), &length, TIMEOUT);
		if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_TIMEOUT) {
			WARNING ("Failed to receive the answer.");
			return DEVICE_STATUS_IO;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, progress);
		}

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

	// When only two 0xFF bytes are received, there are no more dives.
	unsigned char *data = dc_buffer_get_data (buffer);
	if (nbytes == 2 && data[0] == 0xFF && data[1] == 0xFF) {
		dc_buffer_clear (buffer);
		return DEVICE_STATUS_SUCCESS;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (data + nbytes - 2);
	unsigned short ccrc = checksum_add_uint16 (data, nbytes - 2, 0x0);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

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

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 2;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = array_uint16_le (device->version + 12);
	devinfo.firmware = (array_uint16_le (device->version + 8) << 16)
		+ array_uint16_le (device->version + 10);
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 8; ++i) {
		devinfo.serial *= 10;
		devinfo.serial += device->version[i] - '0';
	}
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	unsigned int ndives = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = atomics_cobalt_read_dive (abstract, buffer, (ndives == 0), &progress)) == DEVICE_STATUS_SUCCESS) {
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

		// Adjust the maximum value to take into account the two checksum bytes
		// for the next dive. Since we don't know the total number of dives in
		// advance, we can't calculate the total number of checksum bytes and
		// adjust the maximum on the fly.
		progress.maximum += 2;

		ndives++;
	}

	dc_buffer_free (buffer);

	return rc;
}
