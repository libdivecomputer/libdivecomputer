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

#include "suunto_vyper2.h"
#include "suunto_common2.h"
#include "context-private.h"
#include "checksum.h"
#include "array.h"
#include "timer.h"

#define ISINSTANCE(device) dc_device_isinstance((device), (const dc_device_vtable_t *) &suunto_vyper2_device_vtable)

#define HELO2    0x15

typedef struct suunto_vyper2_device_t {
	suunto_common2_device_t base;
	dc_iostream_t *iostream;
	dc_timer_t *timer;
} suunto_vyper2_device_t;

static dc_status_t suunto_vyper2_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
static dc_status_t suunto_vyper2_device_close (dc_device_t *abstract);

static const suunto_common2_device_vtable_t suunto_vyper2_device_vtable = {
	{
		sizeof(suunto_vyper2_device_t),
		DC_FAMILY_SUUNTO_VYPER2,
		suunto_common2_device_set_fingerprint, /* set_fingerprint */
		suunto_common2_device_read, /* read */
		suunto_common2_device_write, /* write */
		suunto_common2_device_dump, /* dump */
		suunto_common2_device_foreach, /* foreach */
		suunto_common2_device_timesync, /* timesync */
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
suunto_vyper2_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_vyper2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (suunto_vyper2_device_t *) dc_device_allocate (context, &suunto_vyper2_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common2_device_init (&device->base);

	// Set the default values.
	device->iostream = iostream;

	// Create a high resolution timer.
	status = dc_timer_new (&device->timer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to create a high resolution timer.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_iostream_configure (device->iostream, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_timer_free;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_timer_free;
	}

	// Set the DTR line (power supply for the interface).
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_timer_free;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_iostream_sleep (device->iostream, 100);

	// Make sure everything is in a sane state.
	status = dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to reset IO state.");
		goto error_timer_free;
	}

	// Read the version info.
	status = suunto_common2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the version info.");
		goto error_timer_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Version", device->base.version, sizeof (device->base.version));

	// Override the base class values.
	unsigned int model = device->base.version[0];
	if (model == HELO2)
		device->base.layout = &suunto_helo2_layout;
	else
		device->base.layout = &suunto_vyper2_layout;

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_timer_free:
	dc_timer_free (device->timer);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
suunto_vyper2_device_close (dc_device_t *abstract)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	dc_timer_free (device->timer);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper2_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	dc_iostream_sleep (device->iostream, 600);

	// Set RTS to send the command.
	status = dc_iostream_set_rts (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the RTS line.");
		return status;
	}

	// Get the current timestamp.
	dc_usecs_t begin = 0;
	status = dc_timer_now (device->timer, &begin);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Send the command to the dive computer.
	status = dc_iostream_write (device->iostream, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Get the current timestamp.
	dc_usecs_t end = 0;
	status = dc_timer_now (device->timer, &end);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Calculate the elapsed time.
	dc_usecs_t elapsed = end - begin;

	// Calculate the expected duration. A 2 millisecond fudge factor is added
	// because it improves the success rate significantly.
	unsigned int baudrate = 9600;
	unsigned int nbits = 1 + 8 /* databits */ + 1 /* stopbits */ + 0 /* parity */;
	dc_usecs_t expected = (dc_usecs_t) csize * 1000000 * nbits / baudrate + 2000;

	// Wait for the remaining time.
	if (elapsed < expected) {
		dc_usecs_t remaining = expected - elapsed;

		// The remaining time is rounded up to the nearest millisecond
		// because the sleep function doesn't support a higher
		// resolution on all platforms. The higher resolution is
		// pointless anyway, since we already added a fudge factor
		// above.
		dc_iostream_sleep (device->iostream, (remaining + 999) / 1000);
	}

	// Clear RTS to receive the reply.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the RTS line.");
		return status;
	}

	// Receive the answer of the dive computer.
	status = dc_iostream_read (device->iostream, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header of the package.
	if (answer[0] != command[0]) {
		ERROR (abstract->context, "Unexpected answer header.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the size of the package.
	unsigned int len = array_uint16_be (answer + 1);
	if (len + 4 != asize) {
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
