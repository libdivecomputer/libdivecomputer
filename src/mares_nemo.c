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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "mares_nemo.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)


typedef struct mares_nemo_device_t mares_nemo_device_t;

struct mares_nemo_device_t {
	device_t base;
	struct serial *port;
};

static device_status_t mares_nemo_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t mares_nemo_device_close (device_t *abstract);

static const device_backend_t mares_nemo_device_backend = {
	DEVICE_TYPE_MARES_NEMO,
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	mares_nemo_device_dump, /* dump */
	NULL, /* foreach */
	mares_nemo_device_close /* close */
};

static int
device_is_mares_nemo (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_nemo_device_backend;
}


device_status_t
mares_nemo_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_nemo_device_t *device = (mares_nemo_device_t *) malloc (sizeof (mares_nemo_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &mares_nemo_device_backend);

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

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_nemo_device_close (device_t *abstract)
{
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;

	if (! device_is_mares_nemo (abstract))
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
mares_nemo_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	if (! device_is_mares_nemo (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < MARES_NEMO_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	// Receive the header of the package.
	unsigned char header = 0x00;
	for (unsigned int i = 0; i < 20;) {
		int n = serial_read (device->port, &header, 1);
		if (n != 1) {
			WARNING ("Failed to receive the header.");
			return EXITCODE (n);
		}
		if (header == 0xEE) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	unsigned int nbytes = 0;
	while (nbytes < MARES_NEMO_MEMORY_SIZE) {
		// Read the packet.
		unsigned char packet[(MARES_NEMO_PACKET_SIZE + 1) * 2] = {0};
		int n = serial_read (device->port, packet, sizeof (packet));
		if (n != sizeof (packet)) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Verify the checksums of the packet.
		unsigned char crc1 = packet[MARES_NEMO_PACKET_SIZE];
		unsigned char crc2 = packet[MARES_NEMO_PACKET_SIZE * 2 + 1];
		unsigned char ccrc1 = checksum_add_uint8 (packet, MARES_NEMO_PACKET_SIZE, 0x00);
		unsigned char ccrc2 = checksum_add_uint8 (packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE, 0x00);
		if (crc1 == ccrc1 && crc2 == ccrc2) {
			// Both packets have a correct checksum.
			if (memcmp (packet, packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE) != 0) {
				WARNING ("Both packets are not equal.");
				return DEVICE_STATUS_PROTOCOL;
			}
			memcpy (data + nbytes, packet, MARES_NEMO_PACKET_SIZE);
		} else if (crc1 == ccrc1) {
			// Only the first packet has a correct checksum.
			WARNING ("Only the first packet has a correct checksum.");
			memcpy (data + nbytes, packet, MARES_NEMO_PACKET_SIZE);
		} else if (crc2 == ccrc2) {
			// Only the second packet has a correct checksum.
			WARNING ("Only the second packet has a correct checksum.");
			memcpy (data + nbytes, packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE);
		} else {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		nbytes += MARES_NEMO_PACKET_SIZE;
	}

	if (result)
		*result = MARES_NEMO_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}
