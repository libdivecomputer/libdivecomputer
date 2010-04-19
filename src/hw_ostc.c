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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "hw_ostc.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct hw_ostc_device_t {
	device_t base;
	struct serial *port;
	unsigned char fingerprint[5];
} hw_ostc_device_t;

static device_status_t hw_ostc_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t hw_ostc_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t hw_ostc_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t hw_ostc_device_close (device_t *abstract);

static const device_backend_t hw_ostc_device_backend = {
	DEVICE_TYPE_HW_OSTC,
	hw_ostc_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	hw_ostc_device_dump, /* dump */
	hw_ostc_device_foreach, /* foreach */
	hw_ostc_device_close /* close */
};


static int
device_is_hw_ostc (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &hw_ostc_device_backend;
}


device_status_t
hw_ostc_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	hw_ostc_device_t *device = (hw_ostc_device_t *) malloc (sizeof (hw_ostc_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &hw_ostc_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (INFINITE).
	if (serial_set_timeout (device->port, -1) == -1) {
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
hw_ostc_device_close (device_t *abstract)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
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
hw_ostc_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
hw_ostc_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, HW_OSTC_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = HW_OSTC_MEMORY_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'a'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	// Receive the answer.
	unsigned char *data = dc_buffer_get_data (buffer);
	rc = serial_read (device->port, data, HW_OSTC_MEMORY_SIZE);
	if (rc != HW_OSTC_MEMORY_SIZE) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Verify the header.
	unsigned char header[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55};
	if (memcmp (data, header, sizeof (header)) != 0) {
		WARNING ("Unexpected answer header.");
		return DEVICE_STATUS_ERROR;
	}

	// Update and emit a progress event.
	progress.current += HW_OSTC_MEMORY_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
hw_ostc_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (HW_OSTC_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = hw_ostc_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint16_be (data + 264);
	devinfo.serial = array_uint16_le (data + 6);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = hw_ostc_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
hw_ostc_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (abstract && !device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	const unsigned char header[2] = {0xFA, 0xFA};
	const unsigned char footer[2] = {0xFD, 0xFD};

	// Initialize the data stream pointers.
	const unsigned char *current  = data + size;
	const unsigned char *previous = data + size;

	// Search the data stream for header markers.
	while ((current = array_search_backward (data + 266, current - data - 266, header, sizeof (header))) != NULL) {
		// Move the pointer to the begin of the header.
		current -= sizeof (header);

		// Once a header marker is found, start searching
		// for the corresponding footer marker. The search is
		// now limited to the start of the previous dive.
		previous = array_search_forward (current, previous - current, footer, sizeof (footer));

		if (previous) {
			// Move the pointer to the end of the footer.
			previous += sizeof (footer);

			if (device && memcmp (current + 3, device->fingerprint, sizeof (device->fingerprint)) == 0)
				return DEVICE_STATUS_SUCCESS;

			if (callback && !callback (current, previous - current, current + 3, 5, userdata))
				return DEVICE_STATUS_SUCCESS;
		}

		// Prepare for the next iteration.
		previous = current;
	}

	return DEVICE_STATUS_SUCCESS;
}
