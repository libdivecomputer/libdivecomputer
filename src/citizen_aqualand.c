/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Jef Driesen
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

#include <libdivecomputer/citizen_aqualand.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "ringbuffer.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &citizen_aqualand_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

typedef struct citizen_aqualand_device_t {
	dc_device_t base;
	serial_t *port;
	unsigned char fingerprint[8];
} citizen_aqualand_device_t;

static dc_status_t citizen_aqualand_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t citizen_aqualand_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t citizen_aqualand_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t citizen_aqualand_device_close (dc_device_t *abstract);

static const dc_device_vtable_t citizen_aqualand_device_vtable = {
	sizeof(citizen_aqualand_device_t),
	DC_FAMILY_CITIZEN_AQUALAND,
	citizen_aqualand_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	citizen_aqualand_device_dump, /* dump */
	citizen_aqualand_device_foreach, /* foreach */
	citizen_aqualand_device_close /* close */
};


dc_status_t
citizen_aqualand_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	citizen_aqualand_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (citizen_aqualand_device_t *) dc_device_allocate (context, &citizen_aqualand_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	// Set the serial communication protocol (4800 8N1).
	rc = serial_configure (device->port, 4800, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		status = DC_STATUS_IO;
		goto error_close;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		status = DC_STATUS_IO;
		goto error_close;
	}

	// Make sure everything is in a sane state.
	serial_sleep (device->port, 300);
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
citizen_aqualand_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	citizen_aqualand_device_t *device = (citizen_aqualand_device_t*) abstract;

	// Close the device.
	if (serial_close (device->port) == -1) {
		dc_status_set_error(&status, DC_STATUS_IO);
	}

	return status;
}


static dc_status_t
citizen_aqualand_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	citizen_aqualand_device_t *device = (citizen_aqualand_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
citizen_aqualand_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	citizen_aqualand_device_t *device = (citizen_aqualand_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	serial_set_dtr (device->port, 1);

	// Send the init byte.
	const unsigned char init[] = {0x7F};
	int n = serial_write (device->port, init, sizeof (init));
	if (n != sizeof (init)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	serial_sleep(device->port, 1200);

	// Send the command.
	const unsigned char command[] = {0xFF};
	n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	while (1) {
		// Receive the response packet.
		unsigned char answer[32] = {0};
		n = serial_read (device->port, answer, sizeof (answer));
		if (n != sizeof (answer)) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		dc_buffer_append(buffer, answer, sizeof (answer));

		// Send the command.
		n = serial_write (device->port, command, sizeof (command));
		if (n != sizeof (command)) {
			ERROR (abstract->context, "Failed to send the command.");
			return EXITCODE (n);
		}

		if (answer[sizeof(answer) - 1] == 0xFF)
			break;
	}

	serial_set_dtr (device->port, 0);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
citizen_aqualand_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	citizen_aqualand_device_t *device = (citizen_aqualand_device_t *) abstract;

	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = citizen_aqualand_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	unsigned char *data = dc_buffer_get_data (buffer);
	unsigned int   size = dc_buffer_get_size (buffer);

	if (callback && memcmp (data + 0x05, device->fingerprint, sizeof (device->fingerprint)) != 0) {
		callback (data, size, data + 0x05, sizeof (device->fingerprint), userdata);
	}

	dc_buffer_free (buffer);

	return rc;
}
