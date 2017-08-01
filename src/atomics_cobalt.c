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
#ifdef _WIN32
#define NOGDI
#endif
#include <libusb-1.0/libusb.h>
#endif

#include "atomics_cobalt.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &atomics_cobalt_device_vtable)

#define EXITCODE(rc) (rc == LIBUSB_ERROR_TIMEOUT ? DC_STATUS_TIMEOUT : DC_STATUS_IO)

#define VID 0x0471
#define PID 0x0888
#define TIMEOUT 2000

#define FP_OFFSET 20

#define SZ_MEMORY (29 * 64 * 1024)
#define SZ_VERSION 14

typedef struct atomics_cobalt_device_t {
	dc_device_t base;
#ifdef HAVE_LIBUSB
	libusb_context *context;
	libusb_device_handle *handle;
#endif
	unsigned int simulation;
	unsigned char fingerprint[6];
	unsigned char version[SZ_VERSION];
} atomics_cobalt_device_t;

static dc_status_t atomics_cobalt_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t atomics_cobalt_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t atomics_cobalt_device_close (dc_device_t *abstract);

static const dc_device_vtable_t atomics_cobalt_device_vtable = {
	sizeof(atomics_cobalt_device_t),
	DC_FAMILY_ATOMICS_COBALT,
	atomics_cobalt_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	atomics_cobalt_device_foreach, /* foreach */
	NULL, /* timesync */
	atomics_cobalt_device_close /* close */
};


dc_status_t
atomics_cobalt_device_open (dc_device_t **out, dc_context_t *context)
{
#ifdef HAVE_LIBUSB
	dc_status_t status = DC_STATUS_SUCCESS;
	atomics_cobalt_device_t *device = NULL;
#endif

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef HAVE_LIBUSB
	// Allocate memory.
	device = (atomics_cobalt_device_t *) dc_device_allocate (context, &atomics_cobalt_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->context = NULL;
	device->handle = NULL;
	device->simulation = 0;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	int rc = libusb_init (&device->context);
	if (rc < 0) {
		ERROR (context, "Failed to initialize usb support.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	device->handle = libusb_open_device_with_vid_pid (device->context, VID, PID);
	if (device->handle == NULL) {
		ERROR (context, "Failed to open the usb device.");
		status = DC_STATUS_IO;
		goto error_usb_exit;
	}

	rc = libusb_claim_interface (device->handle, 0);
	if (rc < 0) {
		ERROR (context, "Failed to claim the usb interface.");
		status = DC_STATUS_IO;
		goto error_usb_close;
	}

	status = atomics_cobalt_device_version ((dc_device_t *) device, device->version, sizeof (device->version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to identify the dive computer.");
		goto error_usb_close;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_usb_close:
	libusb_close (device->handle);
error_usb_exit:
	libusb_exit (device->context);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}


static dc_status_t
atomics_cobalt_device_close (dc_device_t *abstract)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

#ifdef HAVE_LIBUSB
	libusb_release_interface(device->handle, 0);
	libusb_close (device->handle);
	libusb_exit (device->context);
#endif

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
atomics_cobalt_device_set_simulation (dc_device_t *abstract, unsigned int simulation)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	device->simulation = simulation;

	return DC_STATUS_SUCCESS;
}


dc_status_t
atomics_cobalt_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_VERSION)
		return DC_STATUS_INVALIDARGS;

#ifdef HAVE_LIBUSB
	// Send the command to the dive computer.
	uint8_t bRequest = 0x01;
	int rc = libusb_control_transfer (device->handle,
		LIBUSB_RECIPIENT_DEVICE | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		bRequest, 0, 0, NULL, 0, TIMEOUT);
	if (rc != LIBUSB_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE(rc);
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_INFO, "Write", &bRequest, 1);

	// Receive the answer from the dive computer.
	int length = 0;
	unsigned char packet[SZ_VERSION + 2] = {0};
	rc = libusb_bulk_transfer (device->handle, 0x82,
		packet, sizeof (packet), &length, TIMEOUT);
	if (rc != LIBUSB_SUCCESS || length != sizeof (packet)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE(rc);
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_INFO, "Read", packet, length);

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (packet + SZ_VERSION);
	unsigned short ccrc = checksum_add_uint16 (packet, SZ_VERSION, 0x0);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, packet, SZ_VERSION);

	return DC_STATUS_SUCCESS;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}


static dc_status_t
atomics_cobalt_read_dive (dc_device_t *abstract, dc_buffer_t *buffer, int init, dc_event_progress_t *progress)
{
#ifdef HAVE_LIBUSB
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
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
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE(rc);
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_INFO, "Write", &bRequest, 1);

	unsigned int nbytes = 0;
	while (1) {
		// Receive the answer from the dive computer.
		int length = 0;
		unsigned char packet[8 * 1024] = {0};
		rc = libusb_bulk_transfer (device->handle, 0x82,
			packet, sizeof (packet), &length, TIMEOUT);
		if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_TIMEOUT) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE(rc);
		}

		HEXDUMP (abstract->context, DC_LOGLEVEL_INFO, "Read", packet, length);

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
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
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Check for the minimum length.
	if (nbytes < 2) {
		ERROR (abstract->context, "Data packet is too short.");
		return DC_STATUS_PROTOCOL;
	}

	// When only two 0xFF bytes are received, there are no more dives.
	unsigned char *data = dc_buffer_get_data (buffer);
	if (nbytes == 2 && data[0] == 0xFF && data[1] == 0xFF) {
		dc_buffer_clear (buffer);
		return DC_STATUS_SUCCESS;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (data + nbytes - 2);
	unsigned short ccrc = checksum_add_uint16 (data, nbytes - 2, 0x0);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	// Remove the checksum bytes.
	dc_buffer_slice (buffer, 0, nbytes - 2);

	return DC_STATUS_SUCCESS;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}


static dc_status_t
atomics_cobalt_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_le (device->version + 12);
	devinfo.firmware = (array_uint16_le (device->version + 8) << 16)
		+ array_uint16_le (device->version + 10);
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 8; ++i) {
		devinfo.serial *= 10;
		devinfo.serial += device->version[i] - '0';
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	unsigned int ndives = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = atomics_cobalt_read_dive (abstract, buffer, (ndives == 0), &progress)) == DC_STATUS_SUCCESS) {
		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);

		if (size == 0) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (memcmp (data + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (data, size, data + FP_OFFSET, sizeof (device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
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
