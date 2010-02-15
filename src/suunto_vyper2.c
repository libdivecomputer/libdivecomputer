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

#include "suunto_common2.h"
#include "suunto_vyper2.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct suunto_vyper2_device_t {
	suunto_common2_device_t base;
	struct serial *port;
} suunto_vyper2_device_t;

static device_status_t suunto_vyper2_device_packet (device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
static device_status_t suunto_vyper2_device_close (device_t *abstract);

static const suunto_common2_device_backend_t suunto_vyper2_device_backend = {
	{
		DEVICE_TYPE_SUUNTO_VYPER2,
		suunto_common2_device_set_fingerprint, /* set_fingerprint */
		suunto_common2_device_version, /* version */
		suunto_common2_device_read, /* read */
		suunto_common2_device_write, /* write */
		suunto_common2_device_dump, /* dump */
		suunto_common2_device_foreach, /* foreach */
		suunto_vyper2_device_close /* close */
	},
	suunto_vyper2_device_packet
};

static int
device_is_suunto_vyper2 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == (const device_backend_t *) &suunto_vyper2_device_backend;
}


device_status_t
suunto_vyper2_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t *) malloc (sizeof (suunto_vyper2_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	suunto_common2_device_init (&device->base, &suunto_vyper2_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_device_close (device_t *abstract)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
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
suunto_vyper2_device_packet (device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DEVICE_STATUS_CANCELLED;

	serial_sleep (0x190 + 0xC8);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Wait until all data has been transmitted.
	serial_drain (device->port);

	serial_sleep (0x9);

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	// Receive the answer of the dive computer.
	n = serial_read (device->port, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the header of the package.
	if (answer[0] != command[0]) {
		WARNING ("Unexpected answer header.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the size of the package.
	if (array_uint16_be (answer + 1) + 4 != asize) {
		WARNING ("Unexpected answer size.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the parameters of the package.
	if (memcmp (command + 3, answer + 3, asize - size - 4) != 0) {
		WARNING ("Unexpected answer parameters.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper2_device_reset_maxdepth (device_t *abstract)
{
	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	return suunto_common2_device_reset_maxdepth (abstract);
}
