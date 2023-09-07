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

#include "citizen_aqualand.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "ringbuffer.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &citizen_aqualand_device_vtable)

#define SZ_HEADER 32

typedef struct citizen_aqualand_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[8];
} citizen_aqualand_device_t;

static dc_status_t citizen_aqualand_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t citizen_aqualand_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t citizen_aqualand_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t citizen_aqualand_device_vtable = {
	sizeof(citizen_aqualand_device_t),
	DC_FAMILY_CITIZEN_AQUALAND,
	citizen_aqualand_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	citizen_aqualand_device_dump, /* dump */
	citizen_aqualand_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};


dc_status_t
citizen_aqualand_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
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
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (4800 8N1).
	status = dc_iostream_configure (device->iostream, 4800, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
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
	dc_status_t status = DC_STATUS_SUCCESS;
	citizen_aqualand_device_t *device = (citizen_aqualand_device_t *) abstract;

	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the DTR line.");
		return status;
	}

	// Send the init byte.
	const unsigned char init[] = {0x7F};
	status = dc_iostream_write (device->iostream, init, sizeof (init), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	dc_iostream_sleep(device->iostream, 1200);

	// Send the command.
	const unsigned char command[] = {0xFF};
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	while (1) {
		// Receive the response packet.
		unsigned char answer[32] = {0};
		status = dc_iostream_read (device->iostream, answer, sizeof (answer), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		if (!dc_buffer_append(buffer, answer, sizeof (answer))) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return status;
		}

		// Send the command.
		status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}

		if (answer[sizeof(answer) - 1] == 0xFF)
			break;
	}

	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to clear the DTR line.");
		return status;
	}

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

	if (size < SZ_HEADER) {
		ERROR (abstract->context, "Dive header is too small (%u).", size);
		dc_buffer_free (buffer);
		return DC_STATUS_DATAFORMAT;
	}

	if (callback && memcmp (data + 0x05, device->fingerprint, sizeof (device->fingerprint)) != 0) {
		callback (data, size, data + 0x05, sizeof (device->fingerprint), userdata);
	}

	dc_buffer_free (buffer);

	return rc;
}
