/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libdivecomputer/diverite_nitekq.h>

#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "serial.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &diverite_nitekq_device_vtable)

#define KEEPALIVE  0x3E // '<'
#define BLOCK      0x42 // 'B'
#define DISCONNECT 0x44 // 'D'
#define HANDSHAKE  0x48 // 'H'
#define RESET      0x52 // 'R'
#define UPLOAD     0x55 // 'U'

#define SZ_PACKET 256
#define SZ_MEMORY (128 * SZ_PACKET)
#define SZ_LOGBOOK 6

#define LOGBOOK          0x0320
#define ADDRESS          0x0384
#define EOP              0x03E6
#define RB_PROFILE_BEGIN 0x03E8
#define RB_PROFILE_END   SZ_MEMORY

typedef struct diverite_nitekq_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned char version[32];
	unsigned char fingerprint[SZ_LOGBOOK];
} diverite_nitekq_device_t;

static dc_status_t diverite_nitekq_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t diverite_nitekq_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t diverite_nitekq_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t diverite_nitekq_device_close (dc_device_t *abstract);

static const dc_device_vtable_t diverite_nitekq_device_vtable = {
	sizeof(diverite_nitekq_device_t),
	DC_FAMILY_DIVERITE_NITEKQ,
	diverite_nitekq_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	diverite_nitekq_device_dump, /* dump */
	diverite_nitekq_device_foreach, /* foreach */
	diverite_nitekq_device_close /* close */
};


static dc_status_t
diverite_nitekq_send (diverite_nitekq_device_t *device, unsigned char cmd)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command.
	unsigned char command[] = {cmd};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_receive (diverite_nitekq_device_t *device, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Read the answer.
	status = dc_serial_read (device->port, data, size, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Read the checksum.
	unsigned char checksum[2] = {0};
	status = dc_serial_read (device->port, checksum, sizeof (checksum), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the checksum.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_handshake (diverite_nitekq_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command.
	unsigned char command[] = {HANDSHAKE};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the answer.
	status = dc_serial_read (device->port, device->version, sizeof (device->version), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
diverite_nitekq_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	diverite_nitekq_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (diverite_nitekq_device_t *) dc_device_allocate (context, &diverite_nitekq_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_serial_set_timeout (device->port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_sleep (device->port, 100);
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Perform the handshaking.
	status = diverite_nitekq_handshake (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to handshake.");
		goto error_close;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
diverite_nitekq_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	diverite_nitekq_device_t *device = (diverite_nitekq_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Disconnect.
	diverite_nitekq_send (device, DISCONNECT);

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
diverite_nitekq_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	diverite_nitekq_device_t *device = (diverite_nitekq_device_t*) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	diverite_nitekq_device_t *device = (diverite_nitekq_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char packet[256] = {0};

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SZ_PACKET + SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_PACKET + SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_be (device->version + 0x0A);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Send the upload request. It's not clear whether this request is
	// actually needed, but let's send it anyway.
	rc = diverite_nitekq_send (device, UPLOAD);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	// Receive the response packet. It's currently not used (or needed)
	// for anything, but we prepend it to the main data anyway, in case
	// we ever need it in the future.
	rc = diverite_nitekq_receive (device, packet, sizeof (packet));
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	dc_buffer_append (buffer, packet, sizeof (packet));

	// Update and emit a progress event.
	progress.current += SZ_PACKET;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the request to initiate downloading memory blocks.
	rc = diverite_nitekq_send (device, RESET);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	for (unsigned int i = 0; i < 128; ++i) {
		// Request the next memory block.
		rc = diverite_nitekq_send (device, BLOCK);
		if (rc != DC_STATUS_SUCCESS) {
			return rc;
		}

		// Receive the memory block.
		rc = diverite_nitekq_receive (device, packet, sizeof (packet));
		if (rc != DC_STATUS_SUCCESS) {
			return rc;
		}

		dc_buffer_append (buffer, packet, sizeof (packet));

		// Update and emit a progress event.
		progress.current += SZ_PACKET;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
diverite_nitekq_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = diverite_nitekq_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = diverite_nitekq_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
diverite_nitekq_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	diverite_nitekq_device_t *device = (diverite_nitekq_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_PACKET + SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	// Skip the first packet. We don't need it for anything. It also
	// makes the logic easier because all offsets in the data are
	// relative to the real start of the memory (e.g. excluding this
	// artificial first block).
	data += SZ_PACKET;

	// Allocate memory.
	unsigned char *buffer = (unsigned char *) malloc (SZ_LOGBOOK + RB_PROFILE_END - RB_PROFILE_BEGIN);
	if (buffer == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Get the end of profile pointer.
	unsigned int eop = array_uint16_be(data + EOP);
	if (eop < RB_PROFILE_BEGIN || eop >= RB_PROFILE_END) {
		ERROR (context, "Invalid ringbuffer pointer detected (0x%04x).", eop);
		free (buffer);
		return DC_STATUS_DATAFORMAT;
	}

	// When a new dive is added, the device moves all existing logbook
	// and address entries towards the end, such that the most recent
	// one is always the first one. This is not the case for the profile
	// data, which is added at the end.
	unsigned int previous = eop;
	for (unsigned int i = 0; i < 10; ++i) {
		// Get the pointer to the logbook entry.
		const unsigned char *p = data + LOGBOOK + i * SZ_LOGBOOK;

		// Abort if an empty logbook is found.
		if (array_isequal (p, SZ_LOGBOOK, 0x00))
			break;

		// Get the address of the profile data.
		unsigned int address = array_uint16_be(data + ADDRESS + i * 2);
		if (address < RB_PROFILE_BEGIN || address >= RB_PROFILE_END) {
			ERROR (context, "Invalid ringbuffer pointer detected (0x%04x).", address);
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		// Check the fingerprint data.
		if (device && memcmp (p, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		// Copy the logbook entry.
		memcpy (buffer, p, SZ_LOGBOOK);

		// Copy the profile data.
		unsigned int length = 0;
		if (previous > address) {
			length = previous - address;
			memcpy (buffer + SZ_LOGBOOK, data + address, length);
		} else {
			unsigned int len_a = RB_PROFILE_END - address;
			unsigned int len_b = previous - RB_PROFILE_BEGIN;
			length = len_a + len_b;
			memcpy (buffer + SZ_LOGBOOK, data + address, len_a);
			memcpy (buffer + SZ_LOGBOOK + len_a, data + RB_PROFILE_BEGIN, len_b);
		}

		if (callback && !callback (buffer, length + SZ_LOGBOOK, buffer, SZ_LOGBOOK, userdata)) {
			break;
		}

		previous = address;
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}
