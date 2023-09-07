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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include <libdivecomputer/usb.h>

#include "atomics_cobalt.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &atomics_cobalt_device_vtable)

#define COBALT1 0
#define COBALT2 2

#define VID 0x0471
#define PID 0x0888
#define TIMEOUT 2000

#define FP_OFFSET 20

#define SZ_HEADER 228

#define SZ_MEMORY1 (29 * 64 * 1024) // Cobalt 1
#define SZ_MEMORY2 (41 * 64 * 1024) // Cobalt 2
#define SZ_VERSION 14

typedef struct atomics_cobalt_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned int simulation;
	unsigned char fingerprint[6];
	unsigned char version[SZ_VERSION];
} atomics_cobalt_device_t;

static dc_status_t atomics_cobalt_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t atomics_cobalt_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t atomics_cobalt_device_vtable = {
	sizeof(atomics_cobalt_device_t),
	DC_FAMILY_ATOMICS_COBALT,
	atomics_cobalt_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	atomics_cobalt_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};


dc_status_t
atomics_cobalt_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	atomics_cobalt_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (atomics_cobalt_device_t *) dc_device_allocate (context, &atomics_cobalt_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->simulation = 0;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the timeout for receiving data (2000 ms).
	status = dc_iostream_set_timeout (device->iostream, TIMEOUT);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	status = atomics_cobalt_device_version ((dc_device_t *) device, device->version, sizeof (device->version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to identify the dive computer.");
		goto error_free;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
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
	dc_status_t status = DC_STATUS_SUCCESS;
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_VERSION)
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char bRequest = 0x01;
	dc_usb_control_t control = {
		DC_USB_REQUEST_VENDOR | DC_USB_RECIPIENT_DEVICE | DC_USB_ENDPOINT_OUT, /* bmRequestType */
		bRequest, /* bRequest */
		0, /* wValue */
		0, /* wIndex */
		0, /* wLength */
	};

	status = dc_iostream_ioctl (device->iostream, DC_IOCTL_USB_CONTROL_WRITE, &control, sizeof(control));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer from the dive computer.
	size_t length = 0;
	unsigned char packet[SZ_VERSION + 2] = {0};
	status = dc_iostream_read (device->iostream, packet, sizeof(packet), &length);
	if (status != DC_STATUS_SUCCESS || length != sizeof (packet)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (packet + SZ_VERSION);
	unsigned short ccrc = checksum_add_uint16 (packet, SZ_VERSION, 0x0);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, packet, SZ_VERSION);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
atomics_cobalt_read_dive (dc_device_t *abstract, dc_buffer_t *buffer, int init, dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Adjust the maximum value to take into account the two byte checksum and
	// the 8 byte serial number. Those extra bytes are not stored inside the
	// dive header and are added dynamically during the data transfer. Since we
	// don't know the total number of dives in advance, we can't calculate the
	// total number of extra bytes and adjust the maximum on the fly.
	if (progress) {
		progress->maximum += 2 + 8;
	}

	// Send the command to the dive computer.
	unsigned char bRequest = 0;
	if (device->simulation)
		bRequest = init ? 0x02 : 0x03;
	else
		bRequest = init ? 0x09 : 0x0A;

	dc_usb_control_t control = {
		DC_USB_REQUEST_VENDOR | DC_USB_RECIPIENT_DEVICE | DC_USB_ENDPOINT_OUT, /* bmRequestType */
		bRequest, /* bRequest */
		0, /* wValue */
		0, /* wIndex */
		0, /* wLength */
	};

	status = dc_iostream_ioctl (device->iostream, DC_IOCTL_USB_CONTROL_WRITE, &control, sizeof(control));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	unsigned int nbytes = 0;
	while (1) {
		// Receive the answer from the dive computer.
		size_t length = 0;
		unsigned char packet[8 * 1024] = {0};
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), &length);
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_TIMEOUT) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

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
}


static dc_status_t
atomics_cobalt_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	// Get the model number.
	unsigned int model = array_uint16_le (device->version + 12);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = (model == COBALT2 ? SZ_MEMORY2 : SZ_MEMORY1) + 2;
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

		if (size < SZ_HEADER) {
			ERROR (abstract->context, "Dive header is too small (%u).", size);
			dc_buffer_free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		if (memcmp (data + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (data, size, data + FP_OFFSET, sizeof (device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		ndives++;
	}

	dc_buffer_free (buffer);

	return rc;
}
