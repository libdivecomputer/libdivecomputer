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

#include <libdivecomputer/suunto_vyper2.h>

#include "context-private.h"
#include "suunto_common2.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), (const dc_device_vtable_t *) &suunto_vyper2_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define HELO2    0x15

typedef struct suunto_vyper2_device_t {
	suunto_common2_device_t base;
	serial_t *port;
} suunto_vyper2_device_t;

static dc_status_t suunto_vyper2_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
static dc_status_t suunto_vyper2_device_close (dc_device_t *abstract);

static const suunto_common2_device_vtable_t suunto_vyper2_device_vtable = {
	{
		DC_FAMILY_SUUNTO_VYPER2,
		suunto_common2_device_set_fingerprint, /* set_fingerprint */
		suunto_common2_device_read, /* read */
		suunto_common2_device_write, /* write */
		suunto_common2_device_dump, /* dump */
		suunto_common2_device_foreach, /* foreach */
		suunto_vyper2_device_close /* close */
	},
	suunto_vyper2_device_packet
};

static const suunto_common2_layout_t suunto_vyper2_layout = {
	0x8000, /* memsize */
	0x0011, /* fingerprint */
	0x0023, /* serial */
	0x019A, /* rb_profile_begin */
	0x7FFE /* rb_profile_end */
};

static const suunto_common2_layout_t suunto_helo2_layout = {
	0x8000, /* memsize */
	0x0017, /* fingerprint */
	0x0023, /* serial */
	0x019A, /* rb_profile_begin */
	0x7FFE /* rb_profile_end */
};


dc_status_t
suunto_vyper2_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t *) malloc (sizeof (suunto_vyper2_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common2_device_init (&device->base, context, &suunto_vyper2_device_vtable);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		ERROR (context, "Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (device->port, 100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Enable half-duplex emulation.
	serial_set_halfduplex (device->port, 1);

	// Read the version info.
	dc_status_t status = suunto_common2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the version info.");
		serial_close (device->port);
		free (device);
		return status;
	}

	// Override the base class values.
	unsigned int model = device->base.version[0];
	if (model == HELO2)
		device->base.layout = &suunto_helo2_layout;
	else
		device->base.layout = &suunto_vyper2_layout;

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper2_device_close (dc_device_t *abstract)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

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
suunto_vyper2_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	serial_sleep (device->port, 600);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	// Receive the answer of the dive computer.
	n = serial_read (device->port, answer, asize);
	if (n != asize) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the header of the package.
	if (answer[0] != command[0]) {
		ERROR (abstract->context, "Unexpected answer header.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the size of the package.
	if (array_uint16_be (answer + 1) + 4 != asize) {
		ERROR (abstract->context, "Unexpected answer size.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the parameters of the package.
	if (memcmp (command + 3, answer + 3, asize - size - 4) != 0) {
		ERROR (abstract->context, "Unexpected answer parameters.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_vyper2_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	return suunto_common2_device_version (abstract, data, size);
}


dc_status_t
suunto_vyper2_device_reset_maxdepth (dc_device_t *abstract)
{
	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	return suunto_common2_device_reset_maxdepth (abstract);
}
