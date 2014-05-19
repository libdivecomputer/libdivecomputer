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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include <libdivecomputer/cressi_leonardo.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"
#include "ringbuffer.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &cressi_leonardo_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define SZ_MEMORY 32000

#define RB_LOGBOOK_BEGIN 0x0100
#define RB_LOGBOOK_END   0x1438
#define RB_LOGBOOK_SIZE  0x52
#define RB_LOGBOOK_COUNT ((RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) / RB_LOGBOOK_SIZE)

#define RB_PROFILE_BEGIN 0x1438
#define RB_PROFILE_END   SZ_MEMORY
#define RB_PROFILE_DISTANCE(a,b) ringbuffer_distance (a, b, 0, RB_PROFILE_BEGIN, RB_PROFILE_END)

typedef struct cressi_leonardo_device_t {
	dc_device_t base;
	serial_t *port;
	unsigned char fingerprint[5];
} cressi_leonardo_device_t;

static dc_status_t cressi_leonardo_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t cressi_leonardo_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t cressi_leonardo_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t cressi_leonardo_device_close (dc_device_t *abstract);

static const dc_device_vtable_t cressi_leonardo_device_vtable = {
	DC_FAMILY_CRESSI_LEONARDO,
	cressi_leonardo_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	cressi_leonardo_device_dump, /* dump */
	cressi_leonardo_device_foreach, /* foreach */
	cressi_leonardo_device_close /* close */
};

dc_status_t
cressi_leonardo_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	cressi_leonardo_device_t *device = (cressi_leonardo_device_t *) malloc (sizeof (cressi_leonardo_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &cressi_leonardo_device_vtable);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Clear the DTR and set the RTS line.
	if (serial_set_dtr (device->port, 0) == -1 ||
		serial_set_rts (device->port, 1) == -1) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	serial_sleep (device->port, 100);
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_leonardo_device_close (dc_device_t *abstract)
{
	cressi_leonardo_device_t *device = (cressi_leonardo_device_t *) abstract;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_leonardo_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	cressi_leonardo_device_t *device = (cressi_leonardo_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_leonardo_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	cressi_leonardo_device_t *device = (cressi_leonardo_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command header to the dive computer.
	const unsigned char command[] = {0x7B, 0x31, 0x32, 0x33, 0x44, 0x42, 0x41, 0x7d};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the header packet.
	unsigned char header[7] = {0};
	n = serial_read (device->port, header, sizeof (header));
	if (n != sizeof (header)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the header packet.
	const unsigned char expected[] = {0x7B, 0x21, 0x44, 0x35, 0x42, 0x33, 0x7d};
	if (memcmp (header, expected, sizeof (expected)) != 0) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	unsigned int nbytes = 0;
	while (nbytes < SZ_MEMORY) {
		// Set the minimum packet size.
		unsigned int len = 1024;

		// Increase the packet size if more data is immediately available.
		int available = serial_get_received (device->port);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > SZ_MEMORY)
			len = SZ_MEMORY - nbytes;

		// Read the packet.
		n = serial_read (device->port, data + nbytes, len);
		if (n != len) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	// Receive the trailer packet.
	unsigned char trailer[4] = {0};
	n = serial_read (device->port, trailer, sizeof (trailer));
	if (n != sizeof (trailer)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Convert to a binary checksum.
	unsigned char checksum[2] = {0};
	array_convert_hex2bin (trailer, sizeof (trailer), checksum, sizeof (checksum));

	// Verify the checksum.
	unsigned int csum1 = array_uint16_be (checksum);
	unsigned int csum2 = checksum_crc_ccitt_uint16 (data, SZ_MEMORY);
	if (csum1 != csum2) {
		ERROR (abstract->context, "Unexpected answer bytes.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_leonardo_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = cressi_leonardo_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = data[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_le (data + 1);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = cressi_leonardo_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}

dc_status_t
cressi_leonardo_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	cressi_leonardo_device_t *device = (cressi_leonardo_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	// Locate the most recent dive.
	// The device maintains an internal counter which is incremented for every
	// dive, and the current value at the time of the dive is stored in the
	// dive header. Thus the most recent dive will have the highest value.
	unsigned int count = 0;
	unsigned int latest = 0;
	unsigned int maximum = 0;
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		unsigned int offset = RB_LOGBOOK_BEGIN + i * RB_LOGBOOK_SIZE;

		// Ignore uninitialized header entries.
		if (array_isequal (data + offset, RB_LOGBOOK_SIZE, 0xFF))
			break;

		// Get the internal dive number.
		unsigned int current = array_uint16_le (data + offset);
		if (current == 0xFFFF) {
			WARNING (context, "Unexpected internal dive number found.");
			break;
		}
		if (current > maximum) {
			maximum = current;
			latest = i;
		}

		count++;
	}

	unsigned char *buffer = (unsigned char *) malloc (RB_LOGBOOK_SIZE + RB_PROFILE_END - RB_PROFILE_BEGIN);
	if (buffer == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < count; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = RB_LOGBOOK_BEGIN + idx * RB_LOGBOOK_SIZE;

		// Get the ringbuffer pointers.
		unsigned int header = array_uint16_le (data + offset + 2);
		unsigned int footer = array_uint16_le (data + offset + 4);
		if (header < RB_PROFILE_BEGIN || header + 2 > RB_PROFILE_END ||
			footer < RB_PROFILE_BEGIN || footer + 2 > RB_PROFILE_END)
		{
			ERROR (context, "Invalid ringbuffer pointer detected.");
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the same pointers from the profile.
		unsigned int header2 = array_uint16_le (data + footer);
		unsigned int footer2 = array_uint16_le (data + header);
		if (header2 != header || footer2 != footer) {
			ERROR (context, "Invalid ringbuffer pointer detected.");
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		// Calculate the profile address and length.
		unsigned int address = header + 2;
		unsigned int length = RB_PROFILE_DISTANCE (header, footer) - 2;

		// Check the fingerprint data.
		if (device && memcmp (data + offset + 8, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		// Copy the logbook entry.
		memcpy (buffer, data + offset, RB_LOGBOOK_SIZE);

		// Copy the profile data.
		if (address + length > RB_PROFILE_END) {
			unsigned int len_a = RB_PROFILE_END - address;
			unsigned int len_b = length - len_a;
			memcpy (buffer + RB_LOGBOOK_SIZE, data + address, len_a);
			memcpy (buffer + RB_LOGBOOK_SIZE + len_a, data + RB_PROFILE_BEGIN, len_b);
		} else {
			memcpy (buffer + RB_LOGBOOK_SIZE, data + address, length);
		}

		if (callback && !callback (buffer, RB_LOGBOOK_SIZE + length, buffer + 8, sizeof (device->fingerprint), userdata)) {
			break;
		}
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}
