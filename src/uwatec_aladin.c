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

#include <stdlib.h> // malloc, free
#include <memory.h> // memcpy

#include "uwatec_aladin.h"
#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &uwatec_aladin_device_vtable)

#define SZ_MEMORY 2048

#define RB_PROFILE_BEGIN			0x000
#define RB_PROFILE_END				0x600
#define RB_PROFILE_NEXT(a)			ringbuffer_increment (a, 1, RB_PROFILE_BEGIN, RB_PROFILE_END)
#define RB_PROFILE_DISTANCE(a,b)	ringbuffer_distance (a, b, DC_RINGBUFFER_EMPTY, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define HEADER 4

typedef struct uwatec_aladin_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_aladin_device_t ;

static dc_status_t uwatec_aladin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_aladin_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_aladin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t uwatec_aladin_device_vtable = {
	sizeof(uwatec_aladin_device_t),
	DC_FAMILY_UWATEC_ALADIN,
	uwatec_aladin_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	uwatec_aladin_device_dump, /* dump */
	uwatec_aladin_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};

static dc_status_t
uwatec_aladin_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

dc_status_t
uwatec_aladin_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_aladin_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (uwatec_aladin_device_t *) dc_device_allocate (context, &uwatec_aladin_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Set the serial communication protocol (19200 8N1).
	status = dc_iostream_configure (device->iostream, 19200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Set the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
uwatec_aladin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_aladin_device_t *device = (uwatec_aladin_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_aladin_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_aladin_device_t *device = (uwatec_aladin_device_t*) abstract;

	// Pre-allocate the required amount of memory.
	if (!dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	unsigned char answer[SZ_MEMORY + 2] = {0};

	// Receive the header of the package.
	unsigned int i = 0;
	while (i < 4) {
		if (device_is_cancelled (abstract))
			return DC_STATUS_CANCELLED;

		status = dc_iostream_read (device->iostream, answer + i, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			if (status != DC_STATUS_TIMEOUT)
				return status;
		}

		const unsigned char expected = i < 3 ? 0x55 : 0x00;
		if (status != DC_STATUS_SUCCESS || answer[i] != expected) {
			device_event_emit (abstract, DC_EVENT_WAITING, NULL);
			i = 0;
		} else {
			i++;
		}
	}

	// Fetch the current system time.
	dc_ticks_t now = dc_datetime_now ();

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Receive the remaining part of the package.
	status = dc_iostream_read (device->iostream, answer + 4, sizeof (answer) - 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Unexpected EOF in answer.");
		return status;
	}

	// Update and emit a progress event.
	progress.current += sizeof (answer) - 4;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Reverse the bit order.
	array_reverse_bits (answer, sizeof (answer));

	// Verify the checksum of the package.
	unsigned short crc = array_uint16_le (answer + SZ_MEMORY);
	unsigned short ccrc = checksum_add_uint16 (answer, SZ_MEMORY, 0x0000);
	if (ccrc != crc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	// Store the clock calibration values.
	device->systime = now;
	device->devtime = array_uint32_be (answer + HEADER + 0x7f8);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (abstract, DC_EVENT_CLOCK, &clock);

	dc_buffer_append (buffer, answer, SZ_MEMORY);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_aladin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_aladin_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = data[HEADER + 0x7bc];
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_be (data + HEADER + 0x7ed);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = uwatec_aladin_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
uwatec_aladin_extract_dives (dc_device_t *abstract, const unsigned char* data, unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	uwatec_aladin_device_t *device = (uwatec_aladin_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	// The logbook ring buffer can store up to 37 dives. But
	// if the total number of dives is less, not all logbook
	// entries contain valid data.
	unsigned int ndives = array_uint16_be (data + HEADER + 0x7f2);
	if (ndives > 37)
		ndives = 37;

	// Get the index to the newest logbook entry. This value is
	// normally in the range from 1 to 37 and is converted to
	// a zero based index, taking care not to underflow.
	unsigned int eol = (data[HEADER + 0x7f4] + 37 - 1) % 37;

	// Get the end of the profile ring buffer. This value points
	// to the last byte of the last profile and is incremented
	// one byte to point immediately after the last profile.
	unsigned int eop = RB_PROFILE_NEXT (data[HEADER + 0x7f6] +
		(((data[HEADER + 0x7f7] & 0x0F) >> 1) << 8));

	// Start scanning the profile ringbuffer.
	int profiles = 1;

	// Both ring buffers are traversed backwards to retrieve the most recent
	// dives first. This allows you to download only the new dives and avoids
	// having to rely on the number of profiles in the ring buffer (which
	// is buggy according to the documentation). During the traversal, the
	// previous pointer does always point to the end of the dive data and
	// we move the current pointer backwards until a start marker is found.
	unsigned int previous = eop;
	unsigned int current = eop;
	for (unsigned int i = 0; i < ndives; ++i) {
		// Memory buffer to store one dive.
		unsigned char buffer[18 + RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

		// Get the offset to the current logbook entry.
		unsigned int offset = ((eol + 37 - i) % 37) * 12 + RB_PROFILE_END;

		// Copy the serial number, type and logbook data
		// to the buffer and set the profile length to zero.
		memcpy (buffer + 0, data + HEADER + 0x07ed, 3);
		memcpy (buffer + 3, data + HEADER + 0x07bc, 1);
		memcpy (buffer + 4, data + HEADER + offset, 12);
		memset (buffer + 16, 0, 2);

		// Convert the timestamp from the Aladin (big endian)
		// to the Memomouse format (little endian).
		array_reverse_bytes (buffer + 11, 4);

		unsigned int len = 0;
		if (profiles) {
			// Search the profile ringbuffer for a start marker.
			do {
				if (current == RB_PROFILE_BEGIN)
					current = RB_PROFILE_END;
				current--;

				if (data[HEADER + current] == 0xFF) {
					len = RB_PROFILE_DISTANCE (current, previous);
					previous = current;
					break;
				}
			} while (current != eop);

			if (len >= 1) {
				// Skip the start marker.
				len--;
				unsigned int begin = RB_PROFILE_NEXT (current);
				// Set the profile length.
				buffer[16] = (len     ) & 0xFF;
				buffer[17] = (len >> 8) & 0xFF;
				// Copy the profile data.
				if (begin + len > RB_PROFILE_END) {
					unsigned int a = RB_PROFILE_END - begin;
					unsigned int b = (begin + len) - RB_PROFILE_END;
					memcpy (buffer + 18 + 0, data + HEADER + begin, a);
					memcpy (buffer + 18 + a, data + HEADER,         b);
				} else {
					memcpy (buffer + 18, data + HEADER + begin, len);
				}
			}

			// Since the size of the profile ringbuffer is limited,
			// not all logbook entries will have profile data. Thus,
			// once the end of the profile ringbuffer is reached,
			// there is no need to keep scanning the ringbuffer.
			if (current == eop)
				profiles = 0;
		}

		// Automatically abort when a dive is older than the provided timestamp.
		unsigned int timestamp = array_uint32_le (buffer + 11);
		if (device && timestamp <= device->timestamp)
			return DC_STATUS_SUCCESS;

		if (callback && !callback (buffer, len + 18, buffer + 11, 4, userdata))
			return DC_STATUS_SUCCESS;
	}

	return DC_STATUS_SUCCESS;
}
