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

#include <libdivecomputer/uwatec_meridian.h>

#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "serial.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &uwatec_meridian_device_vtable)

#define ACK 0x11
#define NAK 0x66

typedef struct uwatec_meridian_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_meridian_device_t;

static dc_status_t uwatec_meridian_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_meridian_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_meridian_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t uwatec_meridian_device_close (dc_device_t *abstract);

static const dc_device_vtable_t uwatec_meridian_device_vtable = {
	sizeof(uwatec_meridian_device_t),
	DC_FAMILY_UWATEC_MERIDIAN,
	uwatec_meridian_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	uwatec_meridian_device_dump, /* dump */
	uwatec_meridian_device_foreach, /* foreach */
	uwatec_meridian_device_close /* close */
};


static dc_status_t
uwatec_meridian_transfer (uwatec_meridian_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	assert (csize > 0 && csize <= 255);

	// Build the packet.
	unsigned char packet[255 + 12] = {
		0xFF, 0xFF, 0xFF,
		0xA6, 0x59, 0xBD, 0xC2,
		0x00, /* length */
		0x00, 0x00, 0x00,
		0x00}; /* data and checksum */
	memcpy (packet + 11, command, csize);
	packet[7] = csize;
	packet[11 + csize] = checksum_xor_uint8 (packet + 7, csize + 4, 0x00);

	// Send the packet.
	status = dc_serial_write (device->port, packet, csize + 12, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the echo.
	unsigned char echo[sizeof(packet)];
	status = dc_serial_read (device->port, echo, csize + 12, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the echo.");
		return status;
	}

	// Verify the echo.
	if (memcmp (echo, packet, csize + 12) != 0) {
		WARNING (abstract->context, "Unexpected echo.");
		return DC_STATUS_PROTOCOL;
	}

	// Read the header.
	unsigned char header[6];
	status = dc_serial_read (device->port, header, sizeof (header), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the header.");
		return status;
	}

	// Verify the header.
	if (header[0] != ACK || array_uint32_le (header + 1) != asize + 1 || header[5] != packet[11]) {
		WARNING (abstract->context, "Unexpected header.");
		return DC_STATUS_PROTOCOL;
	}

	// Read the packet.
	status = dc_serial_read (device->port, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet.");
		return status;
	}

	// Read the checksum.
	unsigned char csum = 0x00;
	status = dc_serial_read (device->port, &csum, sizeof (csum), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the checksum.");
		return status;
	}

	// Verify the checksum.
	unsigned char ccsum = 0x00;
	ccsum = checksum_xor_uint8 (header + 1, sizeof (header) - 1, ccsum);
	ccsum = checksum_xor_uint8 (answer, asize, ccsum);
	if (csum != ccsum) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_meridian_handshake (uwatec_meridian_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Command template.
	unsigned char answer[1] = {0};
	unsigned char command[5] = {0x00, 0x10, 0x27, 0, 0};

	// Handshake (stage 1).
	command[0] = 0x1B;
	dc_status_t rc = uwatec_meridian_transfer (device, command, 1, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	command[0] = 0x1C;
	rc = uwatec_meridian_transfer (device, command, 5, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_meridian_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_meridian_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (uwatec_meridian_device_t *) dc_device_allocate (context, &uwatec_meridian_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->port = NULL;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (57600 8N1).
	status = dc_serial_configure (device->port, 57600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (3000ms).
	status  = dc_serial_set_timeout (device->port, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Perform the handshaking.
	uwatec_meridian_handshake (device);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
uwatec_meridian_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_meridian_device_t *device = (uwatec_meridian_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
uwatec_meridian_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_meridian_device_t *device = (uwatec_meridian_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_meridian_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_meridian_device_t *device = (uwatec_meridian_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Command template.
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Read the model number.
	command[0] = 0x10;
	unsigned char model[1] = {0};
	rc = uwatec_meridian_transfer (device, command, 1, model, sizeof (model));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the serial number.
	command[0] = 0x14;
	unsigned char serial[4] = {0};
	rc = uwatec_meridian_transfer (device, command, 1, serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the device clock.
	command[0] = 0x1A;
	unsigned char devtime[4] = {0};
	rc = uwatec_meridian_transfer (device, command, 1, devtime, sizeof (devtime));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (devtime);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Data Length.
	command[0] = 0xC6;
	unsigned char answer[4] = {0};
	rc = uwatec_meridian_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = uwatec_meridian_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0;
	while (nbytes < length) {

		// Read the header.
		unsigned char header[5];
		status = dc_serial_read (device->port, header, sizeof (header), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the header.");
			return status;
		}

		// Get the packet size.
		unsigned int packetsize = array_uint32_le (header);
		if (packetsize < 1 || nbytes + packetsize - 1 > length) {
			WARNING (abstract->context, "Unexpected header.");
			return DC_STATUS_PROTOCOL;
		}

		// Read the packet data.
		status = dc_serial_read (device->port, data + nbytes, packetsize - 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return status;
		}

		// Read the checksum.
		unsigned char csum = 0x00;
		status = dc_serial_read (device->port, &csum, sizeof (csum), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the checksum.");
			return status;
		}

		// Verify the checksum.
		unsigned char ccsum = 0x00;
		ccsum = checksum_xor_uint8 (header, sizeof (header), ccsum);
		ccsum = checksum_xor_uint8 (data + nbytes, packetsize - 1, ccsum);
		if (csum != ccsum) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}


		// Update and emit a progress event.
		progress.current += packetsize - 1;
		device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

		nbytes += packetsize - 1;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_meridian_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_meridian_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_meridian_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
uwatec_meridian_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
