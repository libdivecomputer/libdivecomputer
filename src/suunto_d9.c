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
#include <assert.h> // assert

#include "suunto_d9.h"
#include "suunto_common2.h"
#include "context-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), (const dc_device_vtable_t *) &suunto_d9_device_vtable)

#define D4i         0x19
#define D6i         0x1A
#define D9tx        0x1B
#define DX          0x1C
#define VYPERNOVO   0x1D
#define ZOOPNOVO_A  0x1E
#define ZOOPNOVO_B  0x1F
#define D4F         0x20

typedef struct suunto_d9_device_t {
	suunto_common2_device_t base;
	dc_iostream_t *iostream;
} suunto_d9_device_t;

static dc_status_t suunto_d9_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);

static const suunto_common2_device_vtable_t suunto_d9_device_vtable = {
	{
		sizeof(suunto_d9_device_t),
		DC_FAMILY_SUUNTO_D9,
		suunto_common2_device_set_fingerprint, /* set_fingerprint */
		suunto_common2_device_read, /* read */
		suunto_common2_device_write, /* write */
		suunto_common2_device_dump, /* dump */
		suunto_common2_device_foreach, /* foreach */
		suunto_common2_device_timesync, /* timesync */
		NULL /* close */
	},
	suunto_d9_device_packet
};

static const suunto_common2_layout_t suunto_d9_layout = {
	0x8000, /* memsize */
	0x0011, /* fingerprint */
	0x0023, /* serial */
	0x019A, /* rb_profile_begin */
	0x7FFE /* rb_profile_end */
};

static const suunto_common2_layout_t suunto_d9tx_layout = {
	0x10000, /* memsize */
	0x0013, /* fingerprint */
	0x0024, /* serial */
	0x019A, /* rb_profile_begin */
	0xEBF0 /* rb_profile_end */
};

static const suunto_common2_layout_t suunto_dx_layout = {
	0x10000, /* memsize */
	0x0017, /* fingerprint */
	0x0024, /* serial */
	0x019A, /* rb_profile_begin */
	0xEBF0 /* rb_profile_end */
};


static dc_status_t
suunto_d9_device_autodetect (suunto_d9_device_t *device, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// The list with possible baudrates.
	const int baudrates[] = {9600, 115200};

	// Use the model number as a hint to speedup the detection.
	unsigned int hint = 0;
	if (model == D4i || model == D6i || model == D9tx ||
		model == DX || model == VYPERNOVO || model == ZOOPNOVO_A || model == ZOOPNOVO_B ||
		model == D4F)
		hint = 1;

	for (unsigned int i = 0; i < C_ARRAY_SIZE(baudrates); ++i) {
		// Use the baudrate array as circular array, starting from the hint.
		unsigned int idx = (hint + i) % C_ARRAY_SIZE(baudrates);

		// Adjust the baudrate.
		status = dc_iostream_configure (device->iostream, baudrates[idx], 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to set the terminal attributes.");
			return status;
		}

		// Try reading the version info.
		status = suunto_common2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
		if (status == DC_STATUS_SUCCESS)
			break;
	}

	return status;
}


dc_status_t
suunto_d9_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_d9_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (suunto_d9_device_t *) dc_device_allocate (context, &suunto_d9_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common2_device_init (&device->base);

	// Set the default values.
	device->iostream = iostream;

	// Set the serial communication protocol (9600 8N1).
	status = dc_iostream_configure (device->iostream, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Set the DTR line (power supply for the interface).
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_iostream_sleep (device->iostream, 100);

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Try to autodetect the protocol variant.
	status = suunto_d9_device_autodetect (device, model);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to identify the protocol variant.");
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Version", device->base.version, sizeof (device->base.version));

	// Override the base class values.
	model = device->base.version[0];
	if (model == D4i || model == D6i || model == D9tx ||
		model == VYPERNOVO || model == ZOOPNOVO_A || model == ZOOPNOVO_B ||
		model == D4F)
		device->base.layout = &suunto_d9tx_layout;
	else if (model == DX)
		device->base.layout = &suunto_dx_layout;
	else
		device->base.layout = &suunto_d9_layout;

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
suunto_d9_device_packet (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_d9_device_t *device = (suunto_d9_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Clear RTS to send the command.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to clear RTS.");
		return status;
	}

	// Send the command to the dive computer.
	status = dc_iostream_write (device->iostream, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the echo.
	unsigned char echo[128] = {0};
	assert (sizeof (echo) >= csize);
	status = dc_iostream_read (device->iostream, echo, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the echo.");
		return status;
	}

	// Verify the echo.
	if (memcmp (command, echo, csize) != 0) {
		ERROR (abstract->context, "Unexpected echo.");
		return DC_STATUS_PROTOCOL;
	}

	// Set RTS to receive the reply.
	status = dc_iostream_set_rts (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set RTS.");
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
suunto_d9_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	return suunto_common2_device_version (abstract, data, size);
}


dc_status_t
suunto_d9_device_reset_maxdepth (dc_device_t *abstract)
{
	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	return suunto_common2_device_reset_maxdepth (abstract);
}
