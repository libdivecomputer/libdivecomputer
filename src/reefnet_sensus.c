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
#include "reefnet_sensus.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct reefnet_sensus_device_t {
	device_t base;
	serial_t *port;
	unsigned char handshake[REEFNET_SENSUS_HANDSHAKE_SIZE];
	unsigned int waiting;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} reefnet_sensus_device_t;

static device_status_t reefnet_sensus_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t reefnet_sensus_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t reefnet_sensus_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t reefnet_sensus_device_close (device_t *abstract);

static const device_backend_t reefnet_sensus_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUS,
	reefnet_sensus_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensus_device_dump, /* dump */
	reefnet_sensus_device_foreach, /* foreach */
	reefnet_sensus_device_close /* close */
};

static int
device_is_reefnet_sensus (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensus_device_backend;
}


static device_status_t
reefnet_sensus_cancel (reefnet_sensus_device_t *device)
{
	// Send the command to the device.
	unsigned char command = 0x00;
	int n = serial_write (device->port, &command, 1);
	if (n != 1) {
		WARNING ("Failed to send the cancel command.");
		return EXITCODE (n);
	}

	// The device leaves the waiting state.
	device->waiting = 0;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensus_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t *) malloc (sizeof (reefnet_sensus_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &reefnet_sensus_device_backend);

	// Set the default values.
	device->port = NULL;
	device->waiting = 0;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;
	memset (device->handshake, 0, sizeof (device->handshake));

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (19200 8N1).
	rc = serial_configure (device->port, 19200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
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
reefnet_sensus_device_close (device_t *abstract)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Safely close the connection if the last handshake was
	// successful, but no data transfer was ever initiated.
	if (device->waiting)
		reefnet_sensus_cancel (device);

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
reefnet_sensus_device_get_handshake (device_t *abstract, unsigned char data[], unsigned int size)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUS_HANDSHAKE_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	memcpy (data, device->handshake, REEFNET_SENSUS_HANDSHAKE_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensus_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensus_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
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
reefnet_sensus_handshake (reefnet_sensus_device_t *device)
{
	// Send the command to the device.
	unsigned char command = 0x0A;
	int n = serial_write (device->port, &command, 1);
	if (n != 1) {
		WARNING ("Failed to send the handshake command.");
		return EXITCODE (n);
	}

	// Receive the answer from the device.
	unsigned char handshake[REEFNET_SENSUS_HANDSHAKE_SIZE + 2] = {0};
	n = serial_read (device->port, handshake, sizeof (handshake));
	if (n != sizeof (handshake)) {
		WARNING ("Failed to receive the handshake.");
		return EXITCODE (n);
	}

	// Verify the header of the packet.
	if (handshake[0] != 'O' || handshake[1] != 'K') {
		WARNING ("Unexpected answer header.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// The device is now waiting for a data request.
	device->waiting = 1;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (handshake + 8);

	// Store the handshake packet.
	memcpy (device->handshake, handshake + 2, REEFNET_SENSUS_HANDSHAKE_SIZE);

	// Emit a clock event.
	device_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DEVICE_EVENT_CLOCK, &clock);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = handshake[2] - '0';
	devinfo.firmware = handshake[3] - '0';
	devinfo.serial = array_uint16_le (handshake + 6);
	device_event_emit (&device->base, DEVICE_EVENT_DEVINFO, &devinfo);

	// Wait at least 10 ms to ensures the data line is
	// clear before transmission from the host begins.

	serial_sleep (10);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensus_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, REEFNET_SENSUS_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = 4 + REEFNET_SENSUS_MEMORY_SIZE + 2 + 3;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wake-up the device.
	device_status_t rc = reefnet_sensus_handshake (device);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the command to the device.
	unsigned char command = 0x40;
	int n = serial_write (device->port, &command, 1);
	if (n != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// The device leaves the waiting state.
	device->waiting = 0;

	// Receive the answer from the device.
	unsigned int nbytes = 0;
	unsigned char answer[4 + REEFNET_SENSUS_MEMORY_SIZE + 2 + 3] = {0};
	while (nbytes < sizeof (answer)) {
		unsigned int len = sizeof (answer) - nbytes;
		if (len > 128)
			len = 128;

		n = serial_read (device->port, answer + nbytes, len);
		if (n != len) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	// Verify the headers of the package.
	if (memcmp (answer, "DATA", 4) != 0 ||
		memcmp (answer + sizeof (answer) - 3, "END", 3) != 0) {
		WARNING ("Unexpected answer start or end byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned short crc = array_uint16_le (answer + 4 + REEFNET_SENSUS_MEMORY_SIZE);
	unsigned short ccrc = checksum_add_uint16 (answer + 4, REEFNET_SENSUS_MEMORY_SIZE, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer + 4, REEFNET_SENSUS_MEMORY_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensus_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (REEFNET_SENSUS_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = reefnet_sensus_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = reefnet_sensus_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
reefnet_sensus_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (abstract && !device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Search the entire data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 7 ? size - 7 : 0);
	while (current > 0) {
		current--;
		if (data[current] == 0xFF && data[current + 6] == 0xFE) {
			// Once a start marker is found, start searching
			// for the end of the dive. The search is now
			// limited to the start of the previous dive.
			int found = 0;
			unsigned int nsamples = 0, count = 0;
			unsigned int offset = current + 7; // Skip non-sample data.
			while (offset + 1 <= previous) {
				// Depth (adjusted feet of seawater).
				unsigned char depth = data[offset++];

				// Temperature (degrees Fahrenheit)
				if ((nsamples % 6) == 0) {
					assert (offset + 1 <= previous);
					offset++;
				}

				// Current sample is complete.
				nsamples++;

				// The end of a dive is reached when 17 consecutive  
				// depth samples of less than 3 feet have been found.
				if (depth < 13 + 3) {
					count++;
					if (count == 17) {
						found = 1;
						break;
					}
				} else {
					count = 0;
				}
			}

			// Report an error if no end of dive was found.
			if (!found) {
				WARNING ("No end of dive found.");
				return DEVICE_STATUS_ERROR;
			}

			// Automatically abort when a dive is older than the provided timestamp.
			unsigned int timestamp = array_uint32_le (data + current + 2);
			if (device && timestamp <= device->timestamp)
				return DEVICE_STATUS_SUCCESS;

			if (callback && !callback (data + current, offset - current, data + current + 2, 4, userdata))
				return DEVICE_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 7 ? current - 7 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}
