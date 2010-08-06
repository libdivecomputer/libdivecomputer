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
#include <assert.h> // assert

#include "device-private.h"
#include "suunto_solution.h"
#include "ringbuffer.h"
#include "serial.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define RB_PROFILE_BEGIN			0x020
#define RB_PROFILE_END				0x100

typedef struct suunto_solution_device_t {
	device_t base;
	serial_t *port;
} suunto_solution_device_t;

static device_status_t suunto_solution_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t suunto_solution_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t suunto_solution_device_close (device_t *abstract);

static const device_backend_t suunto_solution_device_backend = {
	DEVICE_TYPE_SUUNTO_SOLUTION,
	NULL, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	suunto_solution_device_dump, /* dump */
	suunto_solution_device_foreach, /* foreach */
	suunto_solution_device_close /* close */
};

static int
device_is_suunto_solution (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_solution_device_backend;
}


device_status_t
suunto_solution_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_solution_device_t *device = (suunto_solution_device_t *) malloc (sizeof (suunto_solution_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_solution_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (1200 8N2).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 2, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the RTS line.
	if (serial_set_rts (device->port, 0)) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_solution_device_close (device_t *abstract)
{
	suunto_solution_device_t *device = (suunto_solution_device_t*) abstract;

	if (! device_is_suunto_solution (abstract))
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
suunto_solution_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	suunto_solution_device_t *device = (suunto_solution_device_t*) abstract;

	if (! device_is_suunto_solution (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, SUUNTO_SOLUTION_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SUUNTO_SOLUTION_MEMORY_SIZE - 1 + 2;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

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
	if (answer[0] != 0x3F) WARNING ("Unexpected answer byte.");

	// Send: 0x4D, 0x01, 0x01
	command[0] = 0x4D;
	command[1] = 0x01;
	command[2] = 0x01;
	serial_write (device->port, command, 3);

	// Update and emit a progress event.
	progress.current += 1;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	data[0] = 0x00;
	for (unsigned int i = 1; i < SUUNTO_SOLUTION_MEMORY_SIZE; ++i) {
		// Receive: 0x01, i, data[i]
		n = serial_read (device->port, answer, 3);
		if (n != 3) return EXITCODE (n);
		if (answer[0] != 0x01 || answer[1] != i) WARNING ("Unexpected answer byte.");

		// Send: i
		command[0] = i;
		serial_write (device->port, command, 1);

		// Receive: data[i]
		n = serial_read (device->port, data + i, 1);
		if (n != 1) return EXITCODE (n);
		if (data[i] != answer[2]) WARNING ("Unexpected answer byte.");

		// Send: 0x0D
		command[0] = 0x0D;
		serial_write (device->port, command, 1);

		// Update and emit a progress event.
		progress.current += 1;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);
	}

	// Receive: 0x02, 0x00, 0x80
	n = serial_read (device->port, answer, 3);
	if (n != 3) return EXITCODE (n);
	if (answer[0] != 0x02 || answer[1] != 0x00 || answer[2] != 0x80) WARNING ("Unexpected answer byte.");

	// Send: 0x80
	command[0] = 0x80;
	serial_write (device->port, command, 1);

	// Receive: 0x80
	n = serial_read (device->port, answer, 1);
	if (n != 1) return EXITCODE (n);
	if (answer[0] != 0x80) WARNING ("Unexpected answer byte.");

	// Send: 0x20
	command[0] = 0x20;
	serial_write (device->port, command, 1);

	// Receive: 0x3F
	n = serial_read (device->port, answer, 1);
	if (n != 1) return EXITCODE (n);
	if (answer[0] != 0x3F) WARNING ("Unexpected answer byte.");

	// Update and emit a progress event.
	progress.current += 1;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_solution_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_suunto_solution (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (SUUNTO_SOLUTION_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = suunto_solution_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_be (data + 0x1D);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = suunto_solution_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
suunto_solution_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	if (abstract && !device_is_suunto_solution (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_SOLUTION_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	// Get the end of the profile ring buffer.
	unsigned int eop = data[0x18];
	assert (eop >= RB_PROFILE_BEGIN && eop < RB_PROFILE_END);
	assert (data[eop] == 0x82);

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
				return DEVICE_STATUS_SUCCESS;

			previous = current;
		}
	}

	assert (data[current] == 0x82);

	return DEVICE_STATUS_SUCCESS;
}
