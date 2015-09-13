/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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

#include <libdivecomputer/reefnet_sensuspro.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &reefnet_sensuspro_device_vtable)

#define SZ_MEMORY    56320
#define SZ_HANDSHAKE 10

typedef struct reefnet_sensuspro_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned char handshake[SZ_HANDSHAKE];
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} reefnet_sensuspro_device_t;

static dc_status_t reefnet_sensuspro_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t reefnet_sensuspro_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t reefnet_sensuspro_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t reefnet_sensuspro_device_close (dc_device_t *abstract);

static const dc_device_vtable_t reefnet_sensuspro_device_vtable = {
	sizeof(reefnet_sensuspro_device_t),
	DC_FAMILY_REEFNET_SENSUSPRO,
	reefnet_sensuspro_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensuspro_device_dump, /* dump */
	reefnet_sensuspro_device_foreach, /* foreach */
	reefnet_sensuspro_device_close /* close */
};


dc_status_t
reefnet_sensuspro_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	reefnet_sensuspro_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (reefnet_sensuspro_device_t *) dc_device_allocate (context, &reefnet_sensuspro_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->port = NULL;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;
	memset (device->handshake, 0, sizeof (device->handshake));

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (19200 8N1).
	status = dc_serial_configure (device->port, 19200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (3000ms).
	status = dc_serial_set_timeout (device->port, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
reefnet_sensuspro_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
reefnet_sensuspro_device_get_handshake (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_HANDSHAKE) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	memcpy (data, device->handshake, SZ_HANDSHAKE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_handshake (reefnet_sensuspro_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Assert a break condition.
	dc_serial_set_break (device->port, 1);

	// Receive the handshake from the dive computer.
	unsigned char handshake[SZ_HANDSHAKE + 2] = {0};
	status = dc_serial_read (device->port, handshake, sizeof (handshake), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the handshake.");
		return status;
	}

	// Clear the break condition again.
	dc_serial_set_break (device->port, 0);

	// Verify the checksum of the handshake packet.
	unsigned short crc = array_uint16_le (handshake + SZ_HANDSHAKE);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (handshake, SZ_HANDSHAKE);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (handshake + 6);

	// Store the handshake packet.
	memcpy (device->handshake, handshake, SZ_HANDSHAKE);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = handshake[0];
	devinfo.firmware = handshake[1];
	devinfo.serial = array_uint16_le (handshake + 4);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->handshake;
	vendor.size = sizeof (device->handshake);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	dc_serial_sleep (device->port, 10);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_send (reefnet_sensuspro_device_t *device, unsigned char command)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Wake-up the device.
	dc_status_t rc = reefnet_sensuspro_handshake (device);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the instruction code to the device.
	status = dc_serial_write (device->port, &command, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensuspro_send  (device, 0xB4);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned char answer[SZ_MEMORY + 2] = {0};
	while (nbytes < sizeof (answer)) {
		unsigned int len = sizeof (answer) - nbytes;
		if (len > 256)
			len = 256;

		status = dc_serial_read (device->port, answer + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	unsigned short crc = array_uint16_le (answer + SZ_MEMORY);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (answer, SZ_MEMORY);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, SZ_MEMORY);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensuspro_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = reefnet_sensuspro_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = reefnet_sensuspro_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
reefnet_sensuspro_device_write_interval (dc_device_t *abstract, unsigned char interval)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (interval < 1 || interval > 127)
		return DC_STATUS_INVALIDARGS;

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensuspro_send  (device, 0xB5);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	dc_serial_sleep (device->port, 10);

	status = dc_serial_write (device->port, &interval, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the data packet.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensuspro_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[2] = {0xFF, 0xFF};

	// Search the entire data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Once a start marker is found, start searching
			// for the corresponding stop marker. The search is
			// now limited to the start of the previous dive.
			int found = 0;
			unsigned int offset = current + 10; // Skip non-sample data.
			while (offset + 2 <= previous) {
				if (memcmp (data + offset, footer, sizeof (footer)) == 0) {
					found = 1;
					break;
				} else {
					offset++;
				}
			}

			// Report an error if no stop marker was found.
			if (!found)
				return DC_STATUS_DATAFORMAT;

			// Automatically abort when a dive is older than the provided timestamp.
			unsigned int timestamp = array_uint32_le (data + current + 6);
			if (device && timestamp <= device->timestamp)
				return DC_STATUS_SUCCESS;

			if (callback && !callback (data + current, offset + 2 - current, data + current + 6, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
