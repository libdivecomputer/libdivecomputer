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

#include "device-private.h"
#include "reefnet_sensuspro.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct reefnet_sensuspro_device_t {
	device_t base;
	serial_t *port;
	unsigned char handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE];
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} reefnet_sensuspro_device_t;

static device_status_t reefnet_sensuspro_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t reefnet_sensuspro_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t reefnet_sensuspro_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t reefnet_sensuspro_device_close (device_t *abstract);

static const device_backend_t reefnet_sensuspro_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUSPRO,
	reefnet_sensuspro_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensuspro_device_dump, /* dump */
	reefnet_sensuspro_device_foreach, /* foreach */
	reefnet_sensuspro_device_close /* close */
};

static int
device_is_reefnet_sensuspro (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensuspro_device_backend;
}


device_status_t
reefnet_sensuspro_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t *) malloc (sizeof (reefnet_sensuspro_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &reefnet_sensuspro_device_backend);

	// Set the default values.
	device->port = NULL;
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

	// Set the timeout for receiving data (3000ms).
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
reefnet_sensuspro_device_close (device_t *abstract)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
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
reefnet_sensuspro_device_get_handshake (device_t *abstract, unsigned char data[], unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSPRO_HANDSHAKE_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	memcpy (data, device->handshake, REEFNET_SENSUSPRO_HANDSHAKE_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensuspro_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
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
reefnet_sensuspro_handshake (reefnet_sensuspro_device_t *device)
{
	// Assert a break condition.
	serial_set_break (device->port, 1);

	// Receive the handshake from the dive computer.
	unsigned char handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE + 2] = {0};
	int rc = serial_read (device->port, handshake, sizeof (handshake));
	if (rc != sizeof (handshake)) {
		WARNING ("Failed to receive the handshake.");
		return EXITCODE (rc);
	}

	// Clear the break condition again.
	serial_set_break (device->port, 0);

	// Verify the checksum of the handshake packet.
	unsigned short crc = array_uint16_le (handshake + REEFNET_SENSUSPRO_HANDSHAKE_SIZE);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (handshake, REEFNET_SENSUSPRO_HANDSHAKE_SIZE);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (handshake + 6);

	// Store the handshake packet.
	memcpy (device->handshake, handshake, REEFNET_SENSUSPRO_HANDSHAKE_SIZE);

	// Emit a clock event.
	device_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DEVICE_EVENT_CLOCK, &clock);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = handshake[0];
	devinfo.firmware = handshake[1];
	devinfo.serial = array_uint16_le (handshake + 4);
	device_event_emit (&device->base, DEVICE_EVENT_DEVINFO, &devinfo);

	serial_sleep (10);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_send (reefnet_sensuspro_device_t *device, unsigned char command)
{
	// Wake-up the device.
	device_status_t rc = reefnet_sensuspro_handshake (device);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the instruction code to the device.
	int n = serial_write (device->port, &command, 1);
	if (n != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, REEFNET_SENSUSPRO_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = REEFNET_SENSUSPRO_MEMORY_SIZE + 2;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensuspro_send  (device, 0xB4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned char answer[REEFNET_SENSUSPRO_MEMORY_SIZE + 2] = {0};
	while (nbytes < sizeof (answer)) {
		unsigned int len = sizeof (answer) - nbytes;
		if (len > 256)
			len = 256;

		int n = serial_read (device->port, answer + nbytes, len);
		if (n != len) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	unsigned short crc = array_uint16_le (answer + REEFNET_SENSUSPRO_MEMORY_SIZE);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (answer, REEFNET_SENSUSPRO_MEMORY_SIZE);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, REEFNET_SENSUSPRO_MEMORY_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (REEFNET_SENSUSPRO_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = reefnet_sensuspro_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = reefnet_sensuspro_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
reefnet_sensuspro_device_write_interval (device_t *abstract, unsigned char interval)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (interval < 1 || interval > 127)
		return DEVICE_STATUS_ERROR;

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensuspro_send  (device, 0xB5);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	serial_sleep (10);

	int n = serial_write (device->port, &interval, 1);
	if (n != 1) {
		WARNING ("Failed to send the new value.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensuspro_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (abstract && !device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
				return DEVICE_STATUS_ERROR;

			// Automatically abort when a dive is older than the provided timestamp.
			unsigned int timestamp = array_uint32_le (data + current + 6);
			if (device && timestamp <= device->timestamp)
				return DEVICE_STATUS_SUCCESS;
		
			if (callback && !callback (data + current, offset + 2 - current, data + current + 6, 4, userdata))
				return DEVICE_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}
