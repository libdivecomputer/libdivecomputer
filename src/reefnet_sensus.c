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

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)


typedef struct reefnet_sensus_device_t reefnet_sensus_device_t;

struct reefnet_sensus_device_t {
	device_t base;
	struct serial *port;
	unsigned int waiting;
};

static device_status_t reefnet_sensus_device_handshake (device_t *abstract, unsigned char *data, unsigned int size);
static device_status_t reefnet_sensus_device_dump (device_t *abstract, unsigned char *data, unsigned int size, unsigned int *result);
static device_status_t reefnet_sensus_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t reefnet_sensus_device_close (device_t *abstract);

static const device_backend_t reefnet_sensus_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUS,
	reefnet_sensus_device_handshake, /* handshake */
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


static device_status_t
reefnet_sensus_device_handshake (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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

#ifndef NDEBUG
	message (
		"Response Header: %c%c\n"
		"Product Code:    %d\n"
		"Product Version: %d\n"
		"Battery:         %d\n"
		"Interval:        %d\n"
		"Device ID:       %d\n"
		"Current Time:    %d\n",
		handshake[0], handshake[1],
		handshake[2], handshake[3],
		handshake[4], handshake[5],
		handshake[6] + (handshake[7] << 8),
		handshake[8] + (handshake[9] << 8) + (handshake[10] << 16) + (handshake[11] << 24));
#endif

	if (size >= REEFNET_SENSUS_HANDSHAKE_SIZE) {
		memcpy (data, handshake + 2, REEFNET_SENSUS_HANDSHAKE_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Wait at least 10 ms to ensures the data line is
	// clear before transmission from the host begins.

	serial_sleep (10);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensus_device_dump (device_t *abstract, unsigned char *data, unsigned int size, unsigned int *result)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, 4 + REEFNET_SENSUS_MEMORY_SIZE + 2 + 3);

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

		progress_event (&progress, DEVICE_EVENT_PROGRESS, len);

		nbytes += len;
	}

	// Verify the headers of the package.
	if (memcmp (answer, "DATA", 4) != 0 ||
		memcmp (answer + sizeof (answer) - 3, "END", 3) != 0) {
		WARNING ("Unexpected answer start or end byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned short crc = 
		 answer[4 + REEFNET_SENSUS_MEMORY_SIZE + 0] +
		(answer[4 + REEFNET_SENSUS_MEMORY_SIZE + 1] << 8);
	unsigned short ccrc = checksum_add_uint16 (answer + 4, REEFNET_SENSUS_MEMORY_SIZE, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	if (size >= REEFNET_SENSUS_MEMORY_SIZE) {
		memcpy (data, answer + 4, REEFNET_SENSUS_MEMORY_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	if (result)
		*result = REEFNET_SENSUS_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensus_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	reefnet_sensus_device_t *device = (reefnet_sensus_device_t*) abstract;

	if (! device_is_reefnet_sensus (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char data[REEFNET_SENSUS_MEMORY_SIZE] = {0};

	device_status_t rc = reefnet_sensus_device_dump (abstract, data, sizeof (data), NULL);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return reefnet_sensus_extract_dives (data, sizeof (data), callback, userdata);
}


device_status_t
reefnet_sensus_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
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

			if (callback && !callback (data + current, offset - current, userdata))
				return DEVICE_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 7 ? current - 7 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}
