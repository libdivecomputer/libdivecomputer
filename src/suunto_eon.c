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

#include <libdivecomputer/suunto_eon.h>

#include "context-private.h"
#include "device-private.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

typedef struct suunto_eon_device_t {
	suunto_common_device_t base;
	serial_t *port;
} suunto_eon_device_t;

static dc_status_t suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_eon_device_close (dc_device_t *abstract);

static const device_backend_t suunto_eon_device_backend = {
	DC_FAMILY_SUUNTO_EON,
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
device_is_suunto_eon (dc_device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_eon_device_backend;
}


dc_status_t
suunto_eon_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	suunto_eon_device_t *device = (suunto_eon_device_t *) malloc (sizeof (suunto_eon_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common_device_init (&device->base, context, &suunto_eon_device_backend);

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
	if (serial_set_timeout (device->port, -1) == -1) {
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
suunto_eon_device_close (dc_device_t *abstract)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DC_STATUS_INVALIDARGS;

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
suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DC_STATUS_INVALIDARGS;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SUUNTO_EON_MEMORY_SIZE)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SUUNTO_EON_MEMORY_SIZE + 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'P'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (rc);
	}

	// Receive the answer.
	unsigned char answer[SUUNTO_EON_MEMORY_SIZE + 1] = {0};
	rc = serial_read (device->port, answer, sizeof (answer));
	if (rc != sizeof (answer)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Update and emit a progress event.
	progress.current += sizeof (answer);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = checksum_add_uint8 (answer, sizeof (answer) - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, SUUNTO_EON_MEMORY_SIZE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SUUNTO_EON_MEMORY_SIZE);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = suunto_eon_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_be (data + 244);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = suunto_eon_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
suunto_eon_device_write_name (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size > 20)
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (rc);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_eon_device_write_interval (dc_device_t *abstract, unsigned char interval)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (rc);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_eon_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t*) abstract;

	if (abstract && !device_is_suunto_eon (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SUUNTO_EON_MEMORY_SIZE)
		return DC_STATUS_DATAFORMAT;

	return suunto_common_extract_dives (device, &suunto_eon_layout, data, callback, userdata);
}
