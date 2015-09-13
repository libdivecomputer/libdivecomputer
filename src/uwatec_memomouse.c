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
#include <assert.h> // assert

#include <libdivecomputer/uwatec_memomouse.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &uwatec_memomouse_device_vtable)

#define PACKETSIZE 126

#define ACK 0x60
#define NAK 0xA8

typedef struct uwatec_memomouse_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_memomouse_device_t;

static dc_status_t uwatec_memomouse_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_memomouse_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_memomouse_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t uwatec_memomouse_device_close (dc_device_t *abstract);

static const dc_device_vtable_t uwatec_memomouse_device_vtable = {
	sizeof(uwatec_memomouse_device_t),
	DC_FAMILY_UWATEC_MEMOMOUSE,
	uwatec_memomouse_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	uwatec_memomouse_device_dump, /* dump */
	uwatec_memomouse_device_foreach, /* foreach */
	uwatec_memomouse_device_close /* close */
};


dc_status_t
uwatec_memomouse_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_memomouse_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (uwatec_memomouse_device_t *) dc_device_allocate (context, &uwatec_memomouse_device_vtable);
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

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_serial_set_timeout (device->port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Clear the DTR line.
	status = dc_serial_set_dtr (device->port, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_close;
	}

	// Clear the RTS line.
	status = dc_serial_set_rts (device->port, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
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
uwatec_memomouse_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
uwatec_memomouse_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_memomouse_read_packet (uwatec_memomouse_device_t *device, unsigned char data[], unsigned int size, unsigned int *result)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	assert (result != NULL);

	// Receive the header of the package.
	status = dc_serial_read (device->port, data, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Reverse the bits.
	array_reverse_bits (data, 1);

	// Verify the header of the package.
	unsigned int len = data[0];
	if (len + 2 > size) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Receive the remaining part of the package.
	status = dc_serial_read (device->port, data + 1, len + 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Reverse the bits.
	array_reverse_bits (data + 1, len + 1);

	// Verify the checksum of the package.
	unsigned char crc = data[len + 1];
	unsigned char ccrc = checksum_xor_uint8 (data, len + 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	*result = len;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_memomouse_read_packet_outer (uwatec_memomouse_device_t *device, unsigned char data[], unsigned int size, unsigned int *result)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = uwatec_memomouse_read_packet (device, data, size, result)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL)
			return rc;

		// Flush the input buffer.
		dc_serial_purge (device->port, DC_DIRECTION_INPUT);

		// Reject the packet.
		unsigned char value = NAK;
		status = dc_serial_write (device->port, &value, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to reject the packet.");
			return status;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_memomouse_read_packet_inner (uwatec_memomouse_device_t *device, dc_buffer_t *buffer, dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned int nbytes = 0;
	unsigned int total = PACKETSIZE;
	while (nbytes < total) {
		// Calculate the packet size.
		unsigned int length = total - nbytes;
		if (length > PACKETSIZE)
			length = PACKETSIZE;

		// Read the packet.
		unsigned char packet[PACKETSIZE + 2] = {0};
		dc_status_t rc = uwatec_memomouse_read_packet_outer (device, packet, length + 2, &length);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Accept the packet.
		unsigned char value = ACK;
		status = dc_serial_write (device->port, &value, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to accept the packet.");
			return status;
		}

		if (nbytes == 0) {
			// The first packet should contain at least
			// the total size of the inner packet.
			if (length < 2) {
				ERROR (abstract->context, "Data packet is too short.");
				return DC_STATUS_PROTOCOL;
			}

			// Calculate the total size of the inner packet.
			total = array_uint16_le (packet + 1) + 3;

			// Pre-allocate the required amount of memory.
			if (!dc_buffer_reserve (buffer, total)) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				return DC_STATUS_NOMEMORY;
			}
		}

		// Update and emit a progress event.
		if (progress) {
			progress->maximum = total;
			progress->current += length;
			device_event_emit (&device->base, DC_EVENT_PROGRESS, progress);
		}

		// Append the packet to the buffer.
		dc_buffer_append (buffer, packet + 1, length);

		nbytes += length;
	}

	// Obtain the pointer to the buffer contents.
	unsigned char *data = dc_buffer_get_data (buffer);

	// Verify the checksum.
	unsigned char crc = data[total - 1];
	unsigned char ccrc = checksum_xor_uint8 (data, total - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	// Discard the header and checksum bytes.
	dc_buffer_slice (buffer, 2, total - 3);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_memomouse_dump_internal (uwatec_memomouse_device_t *device, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	size_t available = 0;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Waiting for greeting message.
	while (dc_serial_get_available (device->port, &available) == DC_STATUS_SUCCESS && available == 0) {
		if (device_is_cancelled (abstract))
			return DC_STATUS_CANCELLED;

		// Flush the input buffer.
		dc_serial_purge (device->port, DC_DIRECTION_INPUT);

		// Reject the packet.
		unsigned char value = NAK;
		status = dc_serial_write (device->port, &value, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to reject the packet.");
			return status;
		}

		dc_serial_sleep (device->port, 300);
	}

	// Read the ID string.
	dc_status_t rc = uwatec_memomouse_read_packet_inner (device, buffer, NULL);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Prepare the command.
	unsigned char command [9] = {
		0x07, 					// Outer packet size.
		0x05, 0x00, 			// Inner packet size.
		0x55, 					// Command byte.
		(device->timestamp      ) & 0xFF,
		(device->timestamp >>  8) & 0xFF,
		(device->timestamp >> 16) & 0xFF,
		(device->timestamp >> 24) & 0xFF,
		0x00}; 					// Outer packet checksum.
	command[8] = checksum_xor_uint8 (command, 8, 0x00);
	array_reverse_bits (command, sizeof (command));

	// Wait a small amount of time before sending the command.
	// Without this delay, the transfer will fail most of the time.
	dc_serial_sleep (device->port, 50);

	// Keep send the command to the device,
	// until the ACK answer is received.
	unsigned char answer = NAK;
	while (answer == NAK) {
		// Flush the input buffer.
		dc_serial_purge (device->port, DC_DIRECTION_INPUT);

		// Send the command to the device.
		status = dc_serial_write (device->port, command, sizeof (command), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}

		// Wait for the answer (ACK).
		status = dc_serial_read (device->port, &answer, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}
	}

	// Verify the answer.
	if (answer != ACK) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Wait for the data packet.
	while (dc_serial_get_available (device->port, &available) == DC_STATUS_SUCCESS && available == 0) {
		if (device_is_cancelled (abstract))
			return DC_STATUS_CANCELLED;

		device_event_emit (&device->base, DC_EVENT_WAITING, NULL);
		dc_serial_sleep (device->port, 100);
	}

	// Fetch the current system time.
	dc_ticks_t now = dc_datetime_now ();

	// Read the data packet.
	rc = uwatec_memomouse_read_packet_inner (device, buffer, &progress);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = now;
	device->devtime = array_uint32_le (dc_buffer_get_data (buffer) + 1);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit ((dc_device_t *) device, DC_EVENT_CLOCK, &clock);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_memomouse_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Give the interface some time to notice the DTR
	// line change from a previous transfer (if any).
	dc_serial_sleep (device->port, 500);

	// Set the DTR line.
	rc = dc_serial_set_dtr (device->port, 1);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the RTS line.");
		return rc;
	}

	// Start the transfer.
	status = uwatec_memomouse_dump_internal (device, buffer);

	// Clear the DTR line again.
	rc = dc_serial_set_dtr (device->port, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the RTS line.");
		return rc;
	}

	return status;
}


static dc_status_t
uwatec_memomouse_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_memomouse_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_memomouse_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
uwatec_memomouse_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Parse the data stream to find the total number of dives.
	unsigned int ndives = 0;
	unsigned int previous = 0;
	unsigned int current = 5;
	while (current + 18 <= size) {
		// Memomouse sends all the data twice. The first time, it sends
		// the data starting from the oldest dive towards the newest dive.
		// Next, it send the same data in reverse order (newest to oldest).
		// We abort the parsing once we detect the first duplicate dive.
		// The second data stream contains always exactly 37 dives, and not
		// all dives have profile data, so it's probably data from the
		// connected Uwatec Aladin (converted to the memomouse format).
		if (previous && memcmp (data + previous, data + current, 18) == 0)
			break;

		// Get the length of the profile data.
		unsigned int len = array_uint16_le (data + current + 16);

		// Check for a buffer overflow.
		if (current + len + 18 > size)
			return DC_STATUS_DATAFORMAT;

		// A memomouse can store data from several dive computers, but only
		// the data of the connected dive computer can be transferred.
		// Therefore, the device info will be the same for all dives, and
		// only needs to be reported once.
		if (abstract && ndives == 0) {
			// Emit a device info event.
			dc_event_devinfo_t devinfo;
			devinfo.model = data[current + 3];
			devinfo.firmware = 0;
			devinfo.serial = array_uint24_be (data + current);
			device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);
		}

		// Move to the next dive.
		previous = current;
		current += len + 18;
		ndives++;
	}

	// Parse the data stream again to return each dive in reverse order
	// (newest dive first). This is less efficient, since the data stream
	// needs to be scanned multiple times, but it makes the behaviour
	// consistent with the equivalent function for the Uwatec Aladin.
	for (unsigned int i = 0; i < ndives; ++i) {
		// Skip the older dives.
		unsigned int offset = 5;
		unsigned int skip = ndives - i - 1;
		while (skip) {
			// Get the length of the profile data.
			unsigned int len = array_uint16_le (data + offset + 16);
			// Move to the next dive.
			offset += len + 18;
			skip--;
		}

		// Get the length of the profile data.
		unsigned int length = array_uint16_le (data + offset + 16);

		if (callback && !callback (data + offset, length + 18, data + offset + 11, 4, userdata))
			return DC_STATUS_SUCCESS;
	}

	return DC_STATUS_SUCCESS;
}
