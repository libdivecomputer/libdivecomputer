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
#include "suunto_eon.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct suunto_eon_device_t {
	suunto_common_device_t base;
	serial_t *port;
} suunto_eon_device_t;

static device_status_t suunto_eon_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t suunto_eon_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t suunto_eon_device_close (device_t *abstract);

static const device_backend_t suunto_eon_device_backend = {
	DEVICE_TYPE_SUUNTO_EON,
	suunto_common_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	suunto_eon_device_dump, /* dump */
	suunto_eon_device_foreach, /* foreach */
	suunto_eon_device_close /* close */
};

static const suunto_common_layout_t suunto_eon_layout = {
	0, /* eop */
	0x100, /* rb_profile_begin */
	SUUNTO_EON_MEMORY_SIZE, /* rb_profile_end */
	6, /* fp_offset */
	3 /* peek */
};


static int
device_is_suunto_eon (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_eon_device_backend;
}


device_status_t
suunto_eon_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_eon_device_t *device = (suunto_eon_device_t *) malloc (sizeof (suunto_eon_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	suunto_common_device_init (&device->base, &suunto_eon_device_backend);

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
	if (serial_set_timeout (device->port, -1) == -1) {
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
suunto_eon_device_close (device_t *abstract)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
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
suunto_eon_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SUUNTO_EON_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SUUNTO_EON_MEMORY_SIZE + 1;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'P'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	// Receive the answer.
	unsigned char answer[SUUNTO_EON_MEMORY_SIZE + 1] = {0};
	rc = serial_read (device->port, answer, sizeof (answer));
	if (rc != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Update and emit a progress event.
	progress.current += sizeof (answer);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = checksum_add_uint8 (answer, sizeof (answer) - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, SUUNTO_EON_MEMORY_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_eon_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SUUNTO_EON_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = suunto_eon_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_be (data + 244);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = suunto_eon_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
suunto_eon_device_write_name (device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size > 20)
		return DEVICE_STATUS_ERROR;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_eon_device_write_interval (device_t *abstract, unsigned char interval)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_eon_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t*) abstract;

	if (abstract && !device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_EON_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	return suunto_common_extract_dives (device, &suunto_eon_layout, data, callback, userdata);
}
