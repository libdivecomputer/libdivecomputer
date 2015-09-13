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

#define ISINSTANCE(device) dc_device_isinstance((device), &suunto_eon_device_vtable)

#define SZ_MEMORY 0x900

typedef struct suunto_eon_device_t {
	suunto_common_device_t base;
	dc_serial_t *port;
} suunto_eon_device_t;

static dc_status_t suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_eon_device_close (dc_device_t *abstract);

static const dc_device_vtable_t suunto_eon_device_vtable = {
	sizeof(suunto_eon_device_t),
	DC_FAMILY_SUUNTO_EON,
	suunto_common_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	suunto_eon_device_dump, /* dump */
	suunto_eon_device_foreach, /* foreach */
	suunto_eon_device_close /* close */
};

static const suunto_common_layout_t suunto_eon_layout = {
	0, /* eop */
	0x100, /* rb_profile_begin */
	SZ_MEMORY, /* rb_profile_end */
	6, /* fp_offset */
	3 /* peek */
};


dc_status_t
suunto_eon_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (suunto_eon_device_t *) dc_device_allocate (context, &suunto_eon_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common_device_init (&device->base);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (1200 8N2).
	status = dc_serial_configure (device->port, 1200, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_serial_set_timeout (device->port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Clear the RTS line.
	status = dc_serial_set_rts (device->port, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		goto error_close;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
suunto_eon_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'P'};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer.
	unsigned int nbytes = 0;
	unsigned char answer[SZ_MEMORY + 1] = {0};
	while (nbytes < sizeof(answer)) {
		// Set the minimum packet size.
		unsigned int len = 64;

		// Increase the packet size if more data is immediately available.
		size_t available = 0;
		status = dc_serial_get_available (device->port, &available);
		if (status == DC_STATUS_SUCCESS && available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > sizeof(answer))
			len = sizeof(answer) - nbytes;

		// Read the packet.
		status = dc_serial_read (device->port, answer + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = checksum_add_uint8 (answer, sizeof (answer) - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, SZ_MEMORY);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
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
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		devinfo.serial *= 100;
		devinfo.serial += bcd2dec (data[244 + i]);
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = suunto_eon_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
suunto_eon_device_write_name (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size > 20)
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_eon_device_write_interval (dc_device_t *abstract, unsigned char interval)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_eon_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	return suunto_common_extract_dives (device, &suunto_eon_layout, data, callback, userdata);
}
