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

#include "device-private.h"
#include "uwatec_memomouse.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"
#include "utils.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define PACKETSIZE 126

#define ACK 0x60
#define NAK 0xA8

typedef struct uwatec_memomouse_device_t {
	device_t base;
	struct serial *port;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_memomouse_device_t;

static device_status_t uwatec_memomouse_device_set_fingerprint (device_t *device, const unsigned char data[], unsigned int size);
static device_status_t uwatec_memomouse_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t uwatec_memomouse_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t uwatec_memomouse_device_close (device_t *abstract);

static const device_backend_t uwatec_memomouse_device_backend = {
	DEVICE_TYPE_UWATEC_MEMOMOUSE,
	uwatec_memomouse_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	uwatec_memomouse_device_dump, /* dump */
	uwatec_memomouse_device_foreach, /* foreach */
	uwatec_memomouse_device_close /* close */
};

static int
device_is_uwatec_memomouse (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_memomouse_device_backend;
}


device_status_t
uwatec_memomouse_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t *) malloc (sizeof (uwatec_memomouse_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &uwatec_memomouse_device_backend);

	// Set the default values.
	device->port = NULL;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the RTS and DTR lines.
	if (serial_set_rts (device->port, 0) == -1 ||
		serial_set_dtr (device->port, 0) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_device_close (device_t *abstract)
{
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;

	if (! device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_memomouse_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;

	if (! device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;

	if (! device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != 4)
		return DEVICE_STATUS_ERROR;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_read_packet (uwatec_memomouse_device_t *device, unsigned char data[], unsigned int size, unsigned int *result)
{
	assert (result != NULL);

	// Receive the header of the package.
	int rc = serial_read (device->port, data, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Reverse the bits.
	array_reverse_bits (data, 1);

	// Verify the header of the package.
	unsigned int len = data[0];
	if (len + 2 > size) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Receive the remaining part of the package.
	rc = serial_read (device->port, data + 1, len + 1);
	if (rc != len + 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Reverse the bits.
	array_reverse_bits (data + 1, len + 1);

	// Verify the checksum of the package.
	unsigned char crc = data[len + 1];
	unsigned char ccrc = checksum_xor_uint8 (data, len + 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	*result = len;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_read_packet_outer (uwatec_memomouse_device_t *device, unsigned char data[], unsigned int size, unsigned int *result)
{
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = uwatec_memomouse_read_packet (device, data, size, result)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet, 
		// and request a new one.
		if (rc != DEVICE_STATUS_PROTOCOL)
			return rc;	

		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);

		// Reject the packet.
		unsigned char value = NAK;
		int n = serial_write (device->port, &value, 1);
		if (n != 1) {
			WARNING ("Failed to reject the packet.");
			return EXITCODE (n);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_read_packet_inner (uwatec_memomouse_device_t *device, dc_buffer_t *buffer, device_progress_t *progress)
{
	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
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
		device_status_t rc = uwatec_memomouse_read_packet_outer (device, packet, length + 2, &length);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Accept the packet.
		unsigned char value = ACK;
		int n = serial_write (device->port, &value, 1);
		if (n != 1) {
			WARNING ("Failed to accept the packet.");
			return EXITCODE (n);
		}

		if (nbytes == 0) {
			// The first packet should contain at least
			// the total size of the inner packet.
			if (length < 2) {
				WARNING ("First package is too small.");
				return DEVICE_STATUS_PROTOCOL;
			}

			// Calculate the total size of the inner packet.
			total = array_uint16_le (packet + 1) + 3;

			// Pre-allocate the required amount of memory.
			if (!dc_buffer_reserve (buffer, total)) {
				WARNING ("Insufficient buffer space available.");
				return DEVICE_STATUS_MEMORY;
			}
		}

		// Update and emit a progress event.
		if (progress) {
			progress->maximum = total;
			progress->current += length;
			device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, progress);
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
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Discard the header and checksum bytes.
	dc_buffer_slice (buffer, 2, total - 3);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_dump_internal (uwatec_memomouse_device_t *device, dc_buffer_t *buffer)
{
	device_t *abstract = (device_t *) device;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

	// Waiting for greeting message.
	while (serial_get_received (device->port) == 0) {
		if (device_is_cancelled (abstract))
			return DEVICE_STATUS_CANCELLED;

		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);

		// Reject the packet.
		unsigned char value = NAK;
		int n = serial_write (device->port, &value, 1);
		if (n != 1) {
			WARNING ("Failed to reject the packet.");
			return EXITCODE (n);
		}

		serial_sleep (300);
	}

	// Read the ID string.
	device_status_t rc = uwatec_memomouse_read_packet_inner (device, buffer, NULL);
	if (rc != DEVICE_STATUS_SUCCESS)
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
	serial_sleep (50);

	// Keep send the command to the device, 
	// until the ACK answer is received.
	unsigned char answer = NAK;
	while (answer == NAK) {
		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
		
		// Send the command to the device.
		int n = serial_write (device->port, command, sizeof (command));
		if (n != sizeof (command)) {
			WARNING ("Failed to send the command.");
			return EXITCODE (n);
		}

		// Wait for the answer (ACK).
		n = serial_read (device->port, &answer, 1);
		if (n != 1) {
			WARNING ("Failed to recieve the answer.");
			return EXITCODE (n);
		}
	}

	// Verify the answer.
	if (answer != ACK) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Wait for the data packet.
	while (serial_get_received (device->port) == 0) {
		if (device_is_cancelled (abstract))
			return DEVICE_STATUS_CANCELLED;

		device_event_emit (&device->base, DEVICE_EVENT_WAITING, NULL);
		serial_sleep (100);
	}

	// Fetch the current system time.
	dc_ticks_t now = dc_datetime_now ();

	// Read the data packet.
	rc = uwatec_memomouse_read_packet_inner (device, buffer, &progress);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = now;
	device->devtime = array_uint32_le (dc_buffer_get_data (buffer) + 1);

	// Emit a clock event.
	device_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit ((device_t *) device, DEVICE_EVENT_CLOCK, &clock);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_memomouse_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	uwatec_memomouse_device_t *device = (uwatec_memomouse_device_t*) abstract;

	if (! device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Give the interface some time to notice the DTR
	// line change from a previous transfer (if any).
	serial_sleep (500);

	// Set the DTR line.
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the RTS line.");
		return DEVICE_STATUS_IO;
	}

	// Start the transfer.
	device_status_t rc = uwatec_memomouse_dump_internal (device, buffer);

	// Clear the DTR line again.
	if (serial_set_dtr (device->port, 0) == -1) {
		WARNING ("Failed to set the RTS line.");
		return DEVICE_STATUS_IO;
	}

	return rc;
}


static device_status_t
uwatec_memomouse_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = uwatec_memomouse_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_memomouse_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
uwatec_memomouse_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	if (abstract && !device_is_uwatec_memomouse (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
			return DEVICE_STATUS_ERROR;

		// A memomouse can store data from several dive computers, but only
		// the data of the connected dive computer can be transferred.
		// Therefore, the device info will be the same for all dives, and
		// only needs to be reported once.
		if (abstract && ndives == 0) {
			// Emit a device info event.
			device_devinfo_t devinfo;
			devinfo.model = data[current + 3];
			devinfo.firmware = 0;
			devinfo.serial = array_uint24_be (data + current);
			device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);
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
			return DEVICE_STATUS_SUCCESS;
	}

	return DEVICE_STATUS_SUCCESS;
}
