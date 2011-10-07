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
#include "suunto_d9.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define D4i      0x19
#define D6i      0x1A
#define D9tx     0x1B

typedef struct suunto_d9_device_t {
	suunto_common2_device_t base;
	serial_t *port;
	unsigned char version[4];
} suunto_d9_device_t;

static device_status_t suunto_d9_device_packet (device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size);
static device_status_t suunto_d9_device_close (device_t *abstract);

static const suunto_common2_device_backend_t suunto_d9_device_backend = {
	{
		DEVICE_TYPE_SUUNTO_D9,
		suunto_common2_device_set_fingerprint, /* set_fingerprint */
		suunto_common2_device_version, /* version */
		suunto_common2_device_read, /* read */
		suunto_common2_device_write, /* write */
		suunto_common2_device_dump, /* dump */
		suunto_common2_device_foreach, /* foreach */
		suunto_d9_device_close /* close */
	},
	suunto_d9_device_packet
};

static const suunto_common2_layout_t suunto_d9_layout = {
	0x8000, /* memsize */
	0x0023, /* serial */
	0x019A, /* rb_profile_begin */
	0x7FFE /* rb_profile_end */
};

static const suunto_common2_layout_t suunto_d9tx_layout = {
	0x10000, /* memsize */
	0x0024, /* serial */
	0x019A, /* rb_profile_begin */
	0xFFFE /* rb_profile_end */
};

static int
device_is_suunto_d9 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == (const device_backend_t *) &suunto_d9_device_backend;
}


static device_status_t
suunto_d9_device_autodetect (suunto_d9_device_t *device, unsigned int model)
{
	device_status_t status = DEVICE_STATUS_SUCCESS;

	// The list with possible baudrates.
	const int baudrates[] = {9600, 115200};

	// Use the model number as a hint to speedup the detection.
	unsigned int hint = 0;
	if (model == D4i || model == D6i || model == D9tx)
		hint = 1;

	for (unsigned int i = 0; i < C_ARRAY_SIZE(baudrates); ++i) {
		// Use the baudrate array as circular array, starting from the hint.
		unsigned int idx = (hint + i) % C_ARRAY_SIZE(baudrates);

		// Adjust the baudrate.
		int rc = serial_configure (device->port, baudrates[idx], 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
		if (rc == -1) {
			WARNING ("Failed to set the terminal attributes.");
			return DEVICE_STATUS_IO;
		}

		// Try reading the version info.
		status = suunto_common2_device_version ((device_t *) device, device->version, sizeof (device->version));
		if (status == DEVICE_STATUS_SUCCESS)
			break;
	}

	return status;
}


device_status_t
suunto_d9_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_d9_device_t *device = (suunto_d9_device_t *) malloc (sizeof (suunto_d9_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	suunto_common2_device_init (&device->base, &suunto_d9_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->version, 0, sizeof (device->version));

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

	// Try to autodetect the protocol variant.
	device_status_t status = suunto_d9_device_autodetect (device, 0);
	if (status != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to identify the protocol variant.");
		serial_close (device->port);
		free (device);
		return status;
	}

	// Override the base class values.
	unsigned int model = device->version[0];
	if (model == D4i || model == D6i || model == D9tx)
		device->base.layout = &suunto_d9tx_layout;
	else
		device->base.layout = &suunto_d9_layout;

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_d9_device_close (device_t *abstract)
{
	suunto_d9_device_t *device = (suunto_d9_device_t*) abstract;

	if (! device_is_suunto_d9 (abstract))
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
suunto_d9_device_packet (device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	suunto_d9_device_t *device = (suunto_d9_device_t *) abstract;

	if (device_is_cancelled (abstract))
		return DEVICE_STATUS_CANCELLED;

	// Clear RTS to send the command.
	serial_set_rts (device->port, 0);

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Wait until all data has been transmitted.
	serial_drain (device->port);

	// Receive the echo.
	unsigned char echo[128] = {0};
	assert (sizeof (echo) >= csize);
	n = serial_read (device->port, echo, csize);
	if (n != csize) {
		WARNING ("Failed to receive the echo.");
		return EXITCODE (n);
	}

	// Verify the echo.
	if (memcmp (command, echo, csize) != 0) {
		WARNING ("Unexpected echo.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Set RTS to receive the reply.
	serial_set_rts (device->port, 1);

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
suunto_d9_device_reset_maxdepth (device_t *abstract)
{
	if (! device_is_suunto_d9 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	return suunto_common2_device_reset_maxdepth (abstract);
}
