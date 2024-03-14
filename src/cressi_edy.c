/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include "cressi_edy.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"
#include "ringbuffer.h"
#include "rbstream.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &cressi_edy_device_vtable)

#define MAXRETRIES        4

#define SZ_PACKET         0x80
#define SZ_PAGE           (SZ_PACKET / 4)

#define SZ_HEADER 32

#define IQ700 0x05
#define EDY   0x08

typedef struct cressi_edy_layout_t {
	unsigned int memsize;
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	unsigned int rb_logbook_offset;
	unsigned int rb_logbook_size;
	unsigned int rb_logbook_begin;
	unsigned int rb_logbook_end;
	unsigned int config;
} cressi_edy_layout_t;

typedef struct cressi_edy_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	const cressi_edy_layout_t *layout;
	unsigned char fingerprint[SZ_PAGE / 2];
	unsigned int model;
} cressi_edy_device_t;

static dc_status_t cressi_edy_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t cressi_edy_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t cressi_edy_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t cressi_edy_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t cressi_edy_device_close (dc_device_t *abstract);

static const dc_device_vtable_t cressi_edy_device_vtable = {
	sizeof(cressi_edy_device_t),
	DC_FAMILY_CRESSI_EDY,
	cressi_edy_device_set_fingerprint, /* set_fingerprint */
	cressi_edy_device_read, /* read */
	NULL, /* write */
	cressi_edy_device_dump, /* dump */
	cressi_edy_device_foreach, /* foreach */
	NULL, /* timesync */
	cressi_edy_device_close /* close */
};

static const cressi_edy_layout_t cressi_edy_layout = {
	0x8000, /* memsize */
	0x3FE0, /* rb_profile_begin */
	0x7F80, /* rb_profile_end */
	0x7F80, /* rb_logbook_offset */
	2,  /* rb_logbook_size */
	0,  /* rb_logbook_begin */
	60, /* rb_logbook_end */
	0x7C, /* config */
};

static const cressi_edy_layout_t tusa_iq700_layout = {
	0x2000, /* memsize */
	0x0000, /* rb_profile_begin */
	0x1F60, /* rb_profile_end */
	0x1F80, /* rb_logbook_offset */
	1,  /* rb_logbook_size */
	0,  /* rb_logbook_begin */
	60, /* rb_logbook_end */
	0x3C, /* config */
};

static dc_status_t
cressi_edy_packet (cressi_edy_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, int trailer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	for (unsigned int i = 0; i < csize; ++i) {
		// Send the command to the device.
		status = dc_iostream_write (device->iostream, command + i, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}

		// Receive the echo.
		unsigned char echo = 0;
		status = dc_iostream_read (device->iostream, &echo, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the echo.");
			return status;
		}

		// Verify the echo.
		if (command[i] != echo) {
			ERROR (abstract->context, "Unexpected echo.");
			return DC_STATUS_PROTOCOL;
		}
	}

	if (asize) {
		// Receive the answer of the device.
		status = dc_iostream_read (device->iostream, answer, asize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the trailer of the packet.
		if (trailer && answer[asize - 1] != 0x45) {
			ERROR (abstract->context, "Unexpected answer trailer byte.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_edy_transfer (cressi_edy_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, int trailer)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = cressi_edy_packet (device, command, csize, answer, asize, trailer)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Delay the next attempt.
		dc_iostream_sleep (device->iostream, 300);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_edy_init1 (cressi_edy_device_t *device)
{
	unsigned char command[3] = {0x41, 0x42, 0x43};
	unsigned char answer[3] = {0};

	return cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
}


static dc_status_t
cressi_edy_init2 (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x44};
	unsigned char answer[1] = {0};

	dc_status_t rc = cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	device->model = answer[0];

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_init3 (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x0C};
	unsigned char answer[1] = {0};

	return cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
}


static dc_status_t
cressi_edy_quit (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x46};

	return cressi_edy_transfer (device, command, sizeof (command), NULL, 0, 0);
}


dc_status_t
cressi_edy_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_edy_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (cressi_edy_device_t *) dc_device_allocate (context, &cressi_edy_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->layout = NULL;
	device->model = 0;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (1200 8N1).
	status = dc_iostream_configure (device->iostream, 1200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
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

	// Make sure everything is in a sane state.
	dc_iostream_sleep(device->iostream, 300);
	dc_iostream_purge(device->iostream, DC_DIRECTION_ALL);

	// Send the init commands.
	cressi_edy_init1 (device);
	cressi_edy_init2 (device);
	cressi_edy_init3 (device);

	if (device->model == IQ700) {
		device->layout = &tusa_iq700_layout;
	} else {
		device->layout = &cressi_edy_layout;
	}

	// Set the serial communication protocol (4800 8N1).
	status = dc_iostream_configure (device->iostream, 4800, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep(device->iostream, 300);
	dc_iostream_purge(device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
cressi_edy_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_edy_device_t *device = (cressi_edy_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the quit command.
	rc = cressi_edy_quit (device);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
cressi_edy_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	cressi_edy_device_t *device = (cressi_edy_device_t*) abstract;

	if ((address % SZ_PAGE != 0) ||
		(size    % SZ_PACKET != 0))
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / SZ_PAGE;
		unsigned char answer[SZ_PACKET + 1] = {0};
		unsigned char command[3] = {0x52,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF}; // low
		dc_status_t rc = cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer, SZ_PACKET);

		nbytes += SZ_PACKET;
		address += SZ_PACKET;
		data += SZ_PACKET;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	cressi_edy_device_t *device = (cressi_edy_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_edy_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	cressi_edy_device_t *device = (cressi_edy_device_t *) abstract;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = 0;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_PACKET);
}


static dc_status_t
cressi_edy_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	cressi_edy_device_t *device = (cressi_edy_device_t *) abstract;
	const cressi_edy_layout_t *layout = device->layout;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_PACKET +
		(layout->rb_profile_end - layout->rb_profile_begin);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = 0;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Read the logbook data.
	unsigned char logbook[SZ_PACKET] = {0};
	dc_status_t rc = cressi_edy_device_read (abstract, layout->rb_logbook_offset, logbook, sizeof (logbook));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the logbook data.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int last  = logbook[layout->config + 0];
	unsigned int first = logbook[layout->config + 1];
	if (first < layout->rb_logbook_begin || first >= layout->rb_logbook_end ||
		last < layout->rb_logbook_begin || last >= layout->rb_logbook_end) {
		if (last == 0xFF)
			return DC_STATUS_SUCCESS;
		ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%02x 0x%02x).", first, last);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of logbook items.
	unsigned int count = ringbuffer_distance (first, last, DC_RINGBUFFER_EMPTY, layout->rb_logbook_begin, layout->rb_logbook_end) + 1;

	// Get the profile pointer.
	unsigned int eop = array_uint_le (logbook + layout->config + 2, layout->rb_logbook_size) * SZ_PAGE + layout->rb_profile_begin;
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
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
		unsigned int current = array_uint_le (logbook + idx * layout->rb_logbook_size, layout->rb_logbook_size) * SZ_PAGE + layout->rb_profile_begin;
		if (current < layout->rb_profile_begin || current >= layout->rb_profile_end) {
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%04x).", current);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, DC_RINGBUFFER_FULL, layout->rb_profile_begin, layout->rb_profile_end);

		// Check for a ringbuffer overflow.
		if (total + length > layout->rb_profile_end - layout->rb_profile_begin) {
			count = i;
			break;
		}

		total += length;

		previous = current;

		if (idx == layout->rb_logbook_begin)
			idx = layout->rb_logbook_end;
		idx--;
	}

	// Update and emit a progress event.
	progress.current += SZ_PACKET;
	progress.maximum = SZ_PACKET + total;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, SZ_PAGE, SZ_PACKET, layout->rb_profile_begin, layout->rb_profile_end, eop, DC_RBSTREAM_BACKWARD);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// Memory buffer for the profile data.
	unsigned char *buffer = (unsigned char *) malloc (total);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		dc_rbstream_free (rbstream);
		return DC_STATUS_NOMEMORY;
	}

	unsigned int offset = total;

	idx = last;
	previous = eop;
	for (unsigned int i = 0; i < count; ++i) {
		// Get the pointer to the profile data.
		unsigned int current = array_uint_le (logbook + idx * layout->rb_logbook_size, layout->rb_logbook_size) * SZ_PAGE + layout->rb_profile_begin;
		if (current < layout->rb_profile_begin || current >= layout->rb_profile_end) {
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%04x).", current);
			dc_rbstream_free (rbstream);
			free(buffer);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, DC_RINGBUFFER_FULL, layout->rb_profile_begin, layout->rb_profile_end);

		// Move to the begin of the current dive.
		offset -= length;

		// Read the dive.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset, length);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		if (length < SZ_HEADER) {
			ERROR (abstract->context, "Dive header is too small (%u).", length);
			dc_rbstream_free (rbstream);
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		unsigned char *p = buffer + offset;

		if (memcmp (p, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		if (callback && !callback (p, length, p, sizeof (device->fingerprint), userdata))
			break;

		previous = current;

		if (idx == layout->rb_logbook_begin)
			idx = layout->rb_logbook_end;
		idx--;
	}

	dc_rbstream_free (rbstream);
	free(buffer);

	return DC_STATUS_SUCCESS;
}
