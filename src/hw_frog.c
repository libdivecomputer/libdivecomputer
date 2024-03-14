/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#include "hw_frog.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "ringbuffer.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &hw_frog_device_vtable)

#define SZ_DISPLAY    15
#define SZ_CUSTOMTEXT 13
#define SZ_VERSION    (SZ_CUSTOMTEXT + 4)

#define RB_LOGBOOK_SIZE  256
#define RB_LOGBOOK_COUNT 256

#define RB_PROFILE_BEGIN 0x000000
#define RB_PROFILE_END   0x200000
#define RB_PROFILE_DISTANCE(a,b) ringbuffer_distance (a, b, DC_RINGBUFFER_EMPTY, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define READY      0x4D
#define HEADER     0x61
#define CLOCK      0x62
#define CUSTOMTEXT 0x63
#define DIVE       0x66
#define IDENTITY   0x69
#define DISPLAY    0x6E
#define INIT       0xBB
#define EXIT       0xFF

typedef struct hw_frog_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[5];
} hw_frog_device_t;

static dc_status_t hw_frog_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t hw_frog_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t hw_frog_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t hw_frog_device_close (dc_device_t *abstract);

static const dc_device_vtable_t hw_frog_device_vtable = {
	sizeof(hw_frog_device_t),
	DC_FAMILY_HW_FROG,
	hw_frog_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	hw_frog_device_foreach, /* foreach */
	hw_frog_device_timesync, /* timesync */
	hw_frog_device_close /* close */
};


static int
hw_frog_strncpy (unsigned char *data, unsigned int size, const char *text)
{
	// Check the maximum length.
	size_t length = (text ? strlen (text) : 0);
	if (length > size) {
		return -1;
	}

	// Copy the text.
	if (length)
		memcpy (data, text, length);

	// Pad with spaces.
	memset (data + length, 0x20, size - length);

	return 0;
}


static dc_status_t
hw_frog_transfer (hw_frog_device_t *device,
                  dc_event_progress_t *progress,
                  unsigned char cmd,
                  const unsigned char input[],
                  unsigned int isize,
                  unsigned char output[],
                  unsigned int osize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command.
	unsigned char command[1] = {cmd};
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	if (cmd != INIT && cmd != HEADER) {
		// Read the echo.
		unsigned char answer[1] = {0};
		status = dc_iostream_read (device->iostream, answer, sizeof (answer), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the echo.");
			return status;
		}

		// Verify the echo.
		if (memcmp (answer, command, sizeof (command)) != 0) {
			ERROR (abstract->context, "Unexpected echo.");
			return DC_STATUS_PROTOCOL;
		}
	}

	if (input) {
		// Send the input data packet.
		status = dc_iostream_write (device->iostream, input, isize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the data packet.");
			return status;
		}
	}

	if (output) {
		unsigned int nbytes = 0;
		while (nbytes < osize) {
			// Set the minimum packet size.
			unsigned int len = 1024;

			// Increase the packet size if more data is immediately available.
			size_t available = 0;
			status = dc_iostream_get_available (device->iostream, &available);
			if (status == DC_STATUS_SUCCESS && available > len)
				len = available;

			// Limit the packet size to the total size.
			if (nbytes + len > osize)
				len = osize - nbytes;

			// Read the packet.
			status = dc_iostream_read (device->iostream, output + nbytes, len, NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the answer.");
				return status;
			}

			// Update and emit a progress event.
			if (progress) {
				progress->current += len;
				device_event_emit ((dc_device_t *) device, DC_EVENT_PROGRESS, progress);
			}

			nbytes += len;
		}
	}

	if (cmd != EXIT) {
		// Read the ready byte.
		unsigned char answer[1] = {0};
		status = dc_iostream_read (device->iostream, answer, sizeof (answer), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the ready byte.");
			return status;
		}

		// Verify the ready byte.
		if (answer[0] != READY) {
			ERROR (abstract->context, "Unexpected ready byte.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_frog_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_frog_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (hw_frog_device_t *) dc_device_allocate (context, &hw_frog_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
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

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Send the init command.
	status = hw_frog_transfer (device, NULL, INIT, NULL, 0, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the command.");
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
hw_frog_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_frog_device_t *device = (hw_frog_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the exit command.
	rc = hw_frog_transfer (device, NULL, EXIT, NULL, 0, NULL, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
hw_frog_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_frog_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size != SZ_VERSION)
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	dc_status_t rc = hw_frog_transfer (device, NULL, IDENTITY, NULL, 0, data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_frog_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = (RB_LOGBOOK_SIZE * RB_LOGBOOK_COUNT) +
		(RB_PROFILE_END - RB_PROFILE_BEGIN);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Download the version data.
	unsigned char id[SZ_VERSION] = {0};
	dc_status_t rc = hw_frog_device_version (abstract, id, sizeof (id));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the version.");
		return rc;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint16_be (id + 2);
	devinfo.serial = array_uint16_le (id + 0);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate memory.
	unsigned char *header = (unsigned char *) malloc (RB_LOGBOOK_SIZE * RB_LOGBOOK_COUNT);
	if (header == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Download the logbook headers.
	rc = hw_frog_transfer (device, &progress, HEADER,
              NULL, 0, header, RB_LOGBOOK_SIZE * RB_LOGBOOK_COUNT);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the header.");
		free (header);
		return rc;
	}

	// Locate the most recent dive.
	// The device maintains an internal counter which is incremented for every
	// dive, and the current value at the time of the dive is stored in the
	// dive header. Thus the most recent dive will have the highest value.
	unsigned int count = 0;
	unsigned int latest = 0;
	unsigned int maximum = 0;
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		unsigned int offset = i * RB_LOGBOOK_SIZE;

		// Ignore uninitialized header entries.
		if (array_isequal (header + offset, RB_LOGBOOK_SIZE, 0xFF))
			break;

		// Get the internal dive number.
		unsigned int current = array_uint16_le (header + offset + 52);
		if (current > maximum) {
			maximum = current;
			latest = i;
		}

		count++;
	}

	// Calculate the total and maximum size.
	unsigned int ndives = 0;
	unsigned int size = 0;
	unsigned int maxsize = 0;
	for (unsigned int i = 0; i < count; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = idx * RB_LOGBOOK_SIZE;

		// Get the ringbuffer pointers.
		unsigned int begin = array_uint24_le (header + offset + 2);
		unsigned int end   = array_uint24_le (header + offset + 5);
		if (begin < RB_PROFILE_BEGIN ||
			begin >= RB_PROFILE_END ||
			end < RB_PROFILE_BEGIN ||
			end >= RB_PROFILE_END)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%06x 0x%06x).", begin, end);
			free (header);
			return DC_STATUS_DATAFORMAT;
		}

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE + RB_PROFILE_DISTANCE (begin, end) - 6;

		// Check the fingerprint data.
		if (memcmp (header + offset + 9, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		if (length > maxsize)
			maxsize = length;
		size += length;
		ndives++;
	}

	// Update and emit a progress event.
	progress.maximum = (RB_LOGBOOK_SIZE * RB_LOGBOOK_COUNT) + size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Finish immediately if there are no dives available.
	if (ndives == 0) {
		free (header);
		return DC_STATUS_SUCCESS;
	}

	// Allocate enough memory for the largest dive.
	unsigned char *profile = (unsigned char *) malloc (maxsize);
	if (profile == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		free (header);
		return DC_STATUS_NOMEMORY;
	}

	// Download the dives.
	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = idx * RB_LOGBOOK_SIZE;

		// Get the ringbuffer pointers.
		unsigned int begin = array_uint24_le (header + offset + 2);
		unsigned int end   = array_uint24_le (header + offset + 5);

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE + RB_PROFILE_DISTANCE (begin, end) - 6;

		// Download the dive.
		unsigned char number[1] = {idx};
		rc = hw_frog_transfer (device, &progress, DIVE,
			number, sizeof (number), profile, length);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			free (profile);
			free (header);
			return rc;
		}

		// Verify the header in the logbook and profile are identical.
		if (memcmp (profile, header + offset, RB_LOGBOOK_SIZE) != 0) {
			ERROR (abstract->context, "Unexpected profile header.");
			free (profile);
			free (header);
			return rc;

		}

		if (callback && !callback (profile, length, profile + 9, sizeof (device->fingerprint), userdata))
			break;
	}

	free (profile);
	free (header);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_frog_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	// Send the command.
	unsigned char packet[6] = {
		datetime->hour, datetime->minute, datetime->second,
		datetime->month, datetime->day, datetime->year - 2000};
	dc_status_t rc = hw_frog_transfer (device, NULL, CLOCK, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_frog_device_display (dc_device_t *abstract, const char *text)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_DISPLAY] = {0};
	if (hw_frog_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	dc_status_t rc = hw_frog_transfer (device, NULL, DISPLAY, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_frog_device_customtext (dc_device_t *abstract, const char *text)
{
	hw_frog_device_t *device = (hw_frog_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_CUSTOMTEXT] = {0};
	if (hw_frog_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	dc_status_t rc = hw_frog_transfer (device, NULL, CUSTOMTEXT, packet, sizeof (packet), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}
