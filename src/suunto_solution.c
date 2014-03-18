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

#include <libdivecomputer/suunto_solution.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "serial.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &suunto_solution_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define SZ_MEMORY 256

#define RB_PROFILE_BEGIN			0x020
#define RB_PROFILE_END				0x100

typedef struct suunto_solution_device_t {
	dc_device_t base;
	serial_t *port;
} suunto_solution_device_t;

static dc_status_t suunto_solution_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t suunto_solution_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_solution_device_close (dc_device_t *abstract);

static const dc_device_vtable_t suunto_solution_device_vtable = {
	DC_FAMILY_SUUNTO_SOLUTION,
	NULL, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	suunto_solution_device_dump, /* dump */
	suunto_solution_device_foreach, /* foreach */
	suunto_solution_device_close /* close */
};


dc_status_t
suunto_solution_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	suunto_solution_device_t *device = (suunto_solution_device_t *) malloc (sizeof (suunto_solution_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &suunto_solution_device_vtable);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (1200 8N2).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 2, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Clear the RTS line.
	if (serial_set_rts (device->port, 0)) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_solution_device_close (dc_device_t *abstract)
{
	suunto_solution_device_t *device = (suunto_solution_device_t*) abstract;

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
suunto_solution_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	suunto_solution_device_t *device = (suunto_solution_device_t*) abstract;

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY - 1 + 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	int n = 0;
	unsigned char command[3] = {0};
	unsigned char answer[3] = {0};

	// Assert DTR
	serial_set_dtr (device->port, 1);

	// Send: 0xFF
	command[0] = 0xFF;
	serial_write (device->port, command, 1);

	// Receive: 0x3F
	n = serial_read (device->port, answer, 1);
	if (n != 1) return EXITCODE (n);
	if (answer[0] != 0x3F)
		WARNING (abstract->context, "Unexpected answer byte.");

	// Send: 0x4D, 0x01, 0x01
	command[0] = 0x4D;
	command[1] = 0x01;
	command[2] = 0x01;
	serial_write (device->port, command, 3);

	// Update and emit a progress event.
	progress.current += 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	data[0] = 0x00;
	for (unsigned int i = 1; i < SZ_MEMORY; ++i) {
		// Receive: 0x01, i, data[i]
		n = serial_read (device->port, answer, 3);
		if (n != 3) return EXITCODE (n);
		if (answer[0] != 0x01 || answer[1] != i)
			WARNING (abstract->context, "Unexpected answer byte.");

		// Send: i
		command[0] = i;
		serial_write (device->port, command, 1);

		// Receive: data[i]
		n = serial_read (device->port, data + i, 1);
		if (n != 1) return EXITCODE (n);
		if (data[i] != answer[2])
			WARNING (abstract->context, "Unexpected answer byte.");

		// Send: 0x0D
		command[0] = 0x0D;
		serial_write (device->port, command, 1);

		// Update and emit a progress event.
		progress.current += 1;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	// Receive: 0x02, 0x00, 0x80
	n = serial_read (device->port, answer, 3);
	if (n != 3) return EXITCODE (n);
	if (answer[0] != 0x02 || answer[1] != 0x00 || answer[2] != 0x80)
		WARNING (abstract->context, "Unexpected answer byte.");

	// Send: 0x80
	command[0] = 0x80;
	serial_write (device->port, command, 1);

	// Receive: 0x80
	n = serial_read (device->port, answer, 1);
	if (n != 1) return EXITCODE (n);
	if (answer[0] != 0x80)
		WARNING (abstract->context, "Unexpected answer byte.");

	// Send: 0x20
	command[0] = 0x20;
	serial_write (device->port, command, 1);

	// Receive: 0x3F
	n = serial_read (device->port, answer, 1);
	if (n != 1) return EXITCODE (n);
	if (answer[0] != 0x3F)
		WARNING (abstract->context, "Unexpected answer byte.");

	// Update and emit a progress event.
	progress.current += 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_solution_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = suunto_solution_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		devinfo.serial *= 100;
		devinfo.serial += bcd2dec (data[0x1D + i]);
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = suunto_solution_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
suunto_solution_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	// Get the end of the profile ring buffer.
	unsigned int eop = data[0x18];
	if (eop < RB_PROFILE_BEGIN ||
		eop >= RB_PROFILE_END ||
		data[eop] != 0x82)
	{
		return DC_STATUS_DATAFORMAT;
	}

	// The profile data is stored backwards in the ringbuffer. To locate
	// the most recent dive, we start from the end of profile marker and
	// traverse the ringbuffer in the opposite direction (forwards).
	// Since the profile data is now processed in the "wrong" direction,
	// it needs to be reversed again.
	unsigned int previous = eop;
	unsigned int current = eop;
	for (unsigned int i = 0; i < RB_PROFILE_END - RB_PROFILE_BEGIN; ++i) {
		// Move forwards through the ringbuffer.
		current++;
		if (current == RB_PROFILE_END)
			current = RB_PROFILE_BEGIN;

		// Check for an end of profile marker.
		if (data[current] == 0x82)
			break;

		// Store the current byte into the buffer. By starting at the
		// end of the buffer, the data is automatically reversed.
		unsigned int idx = RB_PROFILE_END - RB_PROFILE_BEGIN - i - 1;
		buffer[idx] = data[current];

		// Check for an end of dive marker (of the next dive),
		// to find the start of the current dive.
		unsigned int peek = ringbuffer_increment (current, 2, RB_PROFILE_BEGIN, RB_PROFILE_END);
		if (data[peek] == 0x80) {
			unsigned int len = ringbuffer_distance (previous, current, 0, RB_PROFILE_BEGIN, RB_PROFILE_END);

			if (callback && !callback (buffer + idx, len, NULL, 0, userdata))
				return DC_STATUS_SUCCESS;

			previous = current;
		}
	}

	if (data[current] != 0x82)
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}
