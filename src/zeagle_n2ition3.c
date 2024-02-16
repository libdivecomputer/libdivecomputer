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

#include "zeagle_n2ition3.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"
#include "ringbuffer.h"
#include "rbstream.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &zeagle_n2ition3_device_vtable)

#define SZ_MEMORY 0x8000
#define SZ_PACKET 64

#define RB_PROFILE_BEGIN  0x3FA0
#define RB_PROFILE_END    0x7EC0

#define RB_LOGBOOK_OFFSET 0x7EC0
#define RB_LOGBOOK_BEGIN  0
#define RB_LOGBOOK_END    60

typedef struct zeagle_n2ition3_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[16];
} zeagle_n2ition3_device_t;

static dc_status_t zeagle_n2ition3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t zeagle_n2ition3_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t zeagle_n2ition3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t zeagle_n2ition3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t zeagle_n2ition3_device_vtable = {
	sizeof(zeagle_n2ition3_device_t),
	DC_FAMILY_ZEAGLE_N2ITION3,
	zeagle_n2ition3_device_set_fingerprint, /* set_fingerprint */
	zeagle_n2ition3_device_read, /* read */
	NULL, /* write */
	zeagle_n2ition3_device_dump, /* dump */
	zeagle_n2ition3_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};


static dc_status_t
zeagle_n2ition3_packet (zeagle_n2ition3_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	assert (asize >= csize + 5);

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command to the device.
	status = dc_iostream_write (device->iostream, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer of the device.
	status = dc_iostream_read (device->iostream, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the echo.
	if (memcmp (answer, command, csize) != 0) {
		ERROR (abstract->context, "Unexpected echo.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the header and trailer of the packet.
	if (answer[csize] != 0x02 && answer[asize - 1] != 0x03) {
		ERROR (abstract->context, "Unexpected answer header/trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the size of the packet.
	if (array_uint16_le (answer + csize + 1) + csize + 5 != asize) {
		ERROR (abstract->context, "Unexpected answer size.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned char crc = answer[asize - 2];
	unsigned char ccrc = ~checksum_add_uint8 (answer + csize + 3, asize - csize - 5, 0x00) + 1;
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
zeagle_n2ition3_init (zeagle_n2ition3_device_t *device)
{
	unsigned char answer[6 + 13] = {0};
	unsigned char command[6] = {0x02, 0x01, 0x00, 0x41, 0xBF, 0x03};

	return zeagle_n2ition3_packet (device, command, sizeof (command), answer, sizeof (answer));
}

dc_status_t
zeagle_n2ition3_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	zeagle_n2ition3_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (zeagle_n2ition3_device_t *) dc_device_allocate (context, &zeagle_n2ition3_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (4800 8N1).
	status = dc_iostream_configure (device->iostream, 4800, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Send the init commands.
	zeagle_n2ition3_init (device);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
zeagle_n2ition3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
zeagle_n2ition3_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t*) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > SZ_PACKET)
			len = SZ_PACKET;

		// Read the package.
		unsigned char answer[13 + SZ_PACKET + 6] = {0};
		unsigned char command[13] = {0x02, 0x08, 0x00, 0x4D,
				(address     ) & 0xFF, // low
				(address >> 8) & 0xFF, // high
				len, // count
				0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
		command[11] = ~checksum_add_uint8 (command + 3, 8, 0x00) + 1;
		dc_status_t rc = zeagle_n2ition3_packet (device, command, sizeof (command), answer, 13 + len + 6);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 17, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
zeagle_n2ition3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_PACKET);
}


static dc_status_t
zeagle_n2ition3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	zeagle_n2ition3_device_t *device = (zeagle_n2ition3_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = (RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) * 2 + 8 +
		(RB_PROFILE_END - RB_PROFILE_BEGIN);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the configuration data.
	unsigned char config[(RB_LOGBOOK_END - RB_LOGBOOK_BEGIN) * 2 + 8] = {0};
	dc_status_t rc = zeagle_n2ition3_device_read (abstract, RB_LOGBOOK_OFFSET, config, sizeof (config));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the configuration data.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int last  = config[0x7C];
	unsigned int first = config[0x7D];
	if (first < RB_LOGBOOK_BEGIN || first >= RB_LOGBOOK_END ||
		last < RB_LOGBOOK_BEGIN || last >= RB_LOGBOOK_END) {
		if (last == 0xFF)
			return DC_STATUS_SUCCESS;
		ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%02x 0x%02x).", first, last);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of logbook items.
	unsigned int count = ringbuffer_distance (first, last, DC_RINGBUFFER_EMPTY, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END) + 1;

	// Get the profile pointer.
	unsigned int eop = array_uint16_le (config + 0x7E);
	if (eop < RB_PROFILE_BEGIN || eop >= RB_PROFILE_END) {
		ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%04x).", eop);
		return DC_STATUS_DATAFORMAT;
	}

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
		if (current < RB_PROFILE_BEGIN || current >= RB_PROFILE_END) {
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%04x).", current);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, DC_RINGBUFFER_FULL, RB_PROFILE_BEGIN, RB_PROFILE_END);

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
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, 1, SZ_PACKET, RB_PROFILE_BEGIN, RB_PROFILE_END, eop, DC_RBSTREAM_BACKWARD);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// Memory buffer for the profile data.
	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	unsigned int offset = RB_PROFILE_END - RB_PROFILE_BEGIN;

	idx = last;
	previous = eop;
	for (unsigned int i = 0; i < count; ++i) {
		// Get the pointer to the profile data.
		unsigned int current = array_uint16_le (config + 2 * idx);

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, DC_RINGBUFFER_FULL, RB_PROFILE_BEGIN, RB_PROFILE_END);

		// Move to the begin of the current dive.
		offset -= length;

		// Read the dive.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset, length);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			return rc;
		}

		unsigned char *p = buffer + offset;

		if (memcmp (p, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_rbstream_free (rbstream);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (p, length, p, sizeof (device->fingerprint), userdata)) {
			dc_rbstream_free (rbstream);
			return DC_STATUS_SUCCESS;
		}

		previous = current;

		if (idx == RB_LOGBOOK_BEGIN)
			idx = RB_LOGBOOK_END;
		idx--;
	}

	dc_rbstream_free (rbstream);

	return DC_STATUS_SUCCESS;
}
