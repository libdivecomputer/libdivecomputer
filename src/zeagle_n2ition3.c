/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "zeagle_n2ition3.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"
#include "ringbuffer.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define RB_PROFILE_BEGIN  0x3FA0
#define RB_PROFILE_END    0x7EC0

#define RB_LOGBOOK_OFFSET 0x7EC0
#define RB_LOGBOOK_BEGIN  0
#define RB_LOGBOOK_END    60

typedef struct zeagle_n2ition3_device_t {
	device_t base;
	serial_t *port;
	unsigned char fingerprint[16];
} zeagle_n2ition3_device_t;

static device_status_t zeagle_n2ition3_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t zeagle_n2ition3_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t zeagle_n2ition3_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t zeagle_n2ition3_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t zeagle_n2ition3_device_close (device_t *abstract);

static const device_backend_t zeagle_n2ition3_device_backend = {
	DEVICE_TYPE_ZEAGLE_N2ITION3,
	zeagle_n2ition3_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	zeagle_n2ition3_device_read, /* read */
	NULL, /* write */
	zeagle_n2ition3_device_dump, /* dump */
	zeagle_n2ition3_device_foreach, /* foreach */
	zeagle_n2ition3_device_close /* close */
};

static int
device_is_zeagle_n2ition3 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &zeagle_n2ition3_device_backend;
}


static device_status_t
zeagle_n2ition3_packet (zeagle_n2ition3_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	assert (asize >= csize + 5);

	// Send the command to the device.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the answer of the device.
	n = serial_read (device->port, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the echo.
	if (memcmp (answer, command, csize) != 0) {
		WARNING ("Unexpected echo.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the header and trailer of the packet.
	if (answer[csize] != 0x02 && answer[asize - 1] != 0x03) {
		WARNING ("Unexpected answer header/trailer byte.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the size of the packet.
	if (array_uint16_le (answer + csize + 1) + csize + 5 != asize) {
		WARNING ("Unexpected answer size.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned char crc = answer[asize - 2];
	unsigned char ccrc = ~checksum_add_uint8 (answer + csize + 3, asize - csize - 5, 0x00) + 1;
	if (crc != ccrc) {
		WARNING ("Unexpected answer checksum.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}

static device_status_t
zeagle_n2ition3_init (zeagle_n2ition3_device_t *device)
{
	unsigned char answer[6 + 13] = {0};
	unsigned char command[6] = {0x02, 0x01, 0x00, 0x41, 0xBF, 0x03};
	command[11] = ~checksum_add_uint8 (command + 3, 8, 0x00) + 1;

	return zeagle_n2ition3_packet (device, command, sizeof (command), answer, sizeof (answer));
}

device_status_t
zeagle_n2ition3_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t *) malloc (sizeof (zeagle_n2ition3_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &zeagle_n2ition3_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (4800 8N1).
	rc = serial_configure (device->port, 4800, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
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

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Send the init commands.
	zeagle_n2ition3_init (device);

	*out = (device_t *) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
zeagle_n2ition3_device_close (device_t *abstract)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t*) abstract;

	if (! device_is_zeagle_n2ition3 (abstract))
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


static device_status_t
zeagle_n2ition3_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
zeagle_n2ition3_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t*) abstract;

	if (! device_is_zeagle_n2ition3 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $ZEAGLE_N2ITION3_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > ZEAGLE_N2ITION3_PACKET_SIZE)
			len = ZEAGLE_N2ITION3_PACKET_SIZE;

		// Read the package.
		unsigned char answer[13 + ZEAGLE_N2ITION3_PACKET_SIZE + 6] = {0};
		unsigned char command[13] = {0x02, 0x08, 0x00, 0x4D,
				(address     ) & 0xFF, // low
				(address >> 8) & 0xFF, // high
				len, // count
				0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
		command[11] = ~checksum_add_uint8 (command + 3, 8, 0x00) + 1;
		device_status_t rc = zeagle_n2ition3_packet (device, command, sizeof (command), answer, 13 + len + 6);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 17, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
zeagle_n2ition3_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	if (! device_is_zeagle_n2ition3 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, ZEAGLE_N2ITION3_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), ZEAGLE_N2ITION3_PACKET_SIZE);
}


static device_status_t
zeagle_n2ition3_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t *) abstract;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = (RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) * 2 + 8 +
		(RB_PROFILE_END - RB_PROFILE_BEGIN);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Read the configuration data.
	unsigned char config[(RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) * 2 + 8] = {0};
	device_status_t rc = zeagle_n2ition3_device_read (abstract, RB_LOGBOOK_OFFSET, config, sizeof (config));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to read the configuration data.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int last  = config[0x7C];
	unsigned int first = config[0x7D];

	// Get the number of logbook items.
	unsigned int count = ringbuffer_distance (first, last, 0, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END) + 1;

	// Get the profile pointer.
	unsigned int eop = array_uint16_le (config + 0x7E);
	
	// The logbook ringbuffer can store at most 60 dives, even if the profile
	// data could store more (e.g. many small dives). But it's also possible
	// that the profile ringbuffer is filled faster than the logbook ringbuffer
	// (e.g. many large dives). We detect this by checking the total length.
	unsigned int total = 0;
	unsigned int idx = last;
	unsigned int previous = eop;
	for (unsigned int i = 0; i < count; ++i) {
		// Get the pointer to the profile data.
		unsigned int current = array_uint16_le (config + 2 * idx);

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, 1, RB_PROFILE_BEGIN, RB_PROFILE_END);
		
		// Check for a ringbuffer overflow.
		if (total + length > RB_PROFILE_END - RB_PROFILE_BEGIN) {
			count = i;
			break;
		}

		total += length;

		previous = current;

		if (idx == RB_LOGBOOK_BEGIN)
			idx = RB_LOGBOOK_END;
		idx--;
	}

	// Update and emit a progress event.
	progress.current += sizeof (config);
	progress.maximum = (RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) * 2 + 8 + total;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Memory buffer for the profile data.
	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	unsigned int available = 0;
	unsigned int remaining = total;
	unsigned int offset = RB_PROFILE_END - RB_PROFILE_BEGIN;
	
	idx = last;
	previous = eop;
	unsigned int address = previous;
	for (unsigned int i = 0; i < count; ++i) {
		// Get the pointer to the profile data.
		unsigned int current = array_uint16_le (config + 2 * idx);

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, 1, RB_PROFILE_BEGIN, RB_PROFILE_END);

		unsigned nbytes = available;
		while (nbytes < length) {
			if (address == RB_PROFILE_BEGIN)
				address = RB_PROFILE_END;
			
			unsigned int len = ZEAGLE_N2ITION3_PACKET_SIZE;
			if (RB_PROFILE_BEGIN + len > address)
				len = address - RB_PROFILE_BEGIN; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.

			address -= len;
			offset -= len;

			// Read the memory page.
			rc = zeagle_n2ition3_device_read (abstract, address, buffer + offset, len);
			if (rc != DEVICE_STATUS_SUCCESS) {
				WARNING ("Failed to read the memory page.");
				return rc;
			}

			// Update and emit a progress event.
			progress.current += len;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

			nbytes += len;
		}

		remaining -= length;
		available = nbytes - length;
		previous = current;

		unsigned char *p = buffer + offset + available;

		if (memcmp (p, device->fingerprint, sizeof (device->fingerprint)) == 0)
			return DEVICE_STATUS_SUCCESS;

		if (callback && !callback (p, length, p, sizeof (device->fingerprint), userdata))
			return DEVICE_STATUS_SUCCESS;

		if (idx == RB_LOGBOOK_BEGIN)
			idx = RB_LOGBOOK_END;
		idx--;
	}

	return DEVICE_STATUS_SUCCESS;
}
