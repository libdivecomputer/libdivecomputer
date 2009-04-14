/* 
 * libdivecomputer
 * 
 * Copyright (C) 2009 Jef Driesen
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
#include "mares_puck.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define MAXRETRIES 4

typedef struct mares_puck_device_t {
	device_t base;
	struct serial *port;
} mares_puck_device_t;

static device_status_t mares_puck_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t mares_puck_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t mares_puck_device_close (device_t *abstract);

static const device_backend_t mares_puck_device_backend = {
	DEVICE_TYPE_MARES_PUCK,
	NULL, /* set_fingerprint */
	NULL, /* version */
	mares_puck_device_read, /* read */
	NULL, /* write */
	mares_puck_device_dump, /* dump */
	NULL, /* foreach */
	mares_puck_device_close /* close */
};

static int
device_is_mares_puck (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_puck_device_backend;
}


device_status_t
mares_puck_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_puck_device_t *device = (mares_puck_device_t *) malloc (sizeof (mares_puck_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &mares_puck_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the DTR/RTS lines.
	if (serial_set_dtr (device->port, 0) == -1 ||
		serial_set_rts (device->port, 0) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_puck_device_close (device_t *abstract)
{
	mares_puck_device_t *device = (mares_puck_device_t*) abstract;

	if (! device_is_mares_puck (abstract))
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


static void
mares_puck_convert_binary_to_ascii (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize)
{
	assert (osize == 2 * isize);

	const unsigned char ascii[] = {
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

	for (unsigned int i = 0; i < isize; ++i) {
		// Set the most-significant nibble.
		unsigned char msn = (input[i] >> 4) & 0x0F;
		output[i * 2 + 0] = ascii[msn];

		// Set the least-significant nibble.
		unsigned char lsn = input[i] & 0x0F;
		output[i * 2 + 1] = ascii[lsn];
	}
}


static void
mares_puck_convert_ascii_to_binary (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize)
{
	assert (isize == 2 * osize);

	for (unsigned int i = 0; i < osize; ++i) {
		unsigned char value = 0;
		for (unsigned int j = 0; j < 2; ++j) {
			unsigned char number = 0;
			unsigned char ascii = input[i * 2 + j];
			if (ascii >= '0' && ascii <= '9')
				number = ascii - '0';
			else if (ascii >= 'A' && ascii <= 'F')
				number = 10 + ascii - 'A';
			else if (ascii >= 'a' && ascii <= 'f')
				number = 10 + ascii - 'a';
			else
				WARNING ("Invalid charachter.");

			value <<= 4;
			value += number;
		}
		output[i] = value;
	}
}


static void
mares_puck_make_ascii (const unsigned char raw[], unsigned int rsize, unsigned char ascii[], unsigned int asize)
{
	assert (asize == 2 * (rsize + 2));

	// Header
	ascii[0] = '<';

	// Data
	mares_puck_convert_binary_to_ascii (raw, rsize, ascii + 1, 2 * rsize);

	// Checksum
	unsigned char checksum = checksum_add_uint8 (ascii + 1, 2 * rsize, 0x00);
	mares_puck_convert_binary_to_ascii (&checksum, 1, ascii + 1 + 2 * rsize, 2);

	// Trailer
	ascii[asize - 1] = '>';
}


static device_status_t
mares_puck_packet (mares_puck_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	// Send the command to the device.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the answer of the device.
	n = serial_read (device->port, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the header and trailer of the packet.
	if (answer[0] != '<' || answer[asize - 1] != '>') {
		WARNING ("Unexpected answer header/trailer byte.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned char crc = 0;
	unsigned char ccrc = checksum_add_uint8 (answer + 1, asize - 4, 0x00);
	mares_puck_convert_ascii_to_binary (answer + asize - 3, 2, &crc, 1);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}
	
	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_puck_transfer (mares_puck_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = mares_puck_packet (device, command, csize, answer, asize)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DEVICE_STATUS_PROTOCOL && rc != DEVICE_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;
	}

	return rc;
}


static device_status_t
mares_puck_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	mares_puck_device_t *device = (mares_puck_device_t*) abstract;

	if (! device_is_mares_puck (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $MARES_PUCK_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > MARES_PUCK_PACKET_SIZE)
			len = MARES_PUCK_PACKET_SIZE;

		// Build the raw command.
		unsigned char raw[] = {0x51,
			(address     ) & 0xFF, // Low
			(address >> 8) & 0xFF, // High
			len}; // Count

		// Build the ascii command.
		unsigned char command[2 * (sizeof (raw) + 2)] = {0};
		mares_puck_make_ascii (raw, sizeof (raw), command, sizeof (command));

		// Send the command and receive the answer.
		unsigned char answer[2 * (MARES_PUCK_PACKET_SIZE + 2)] = {0};
		device_status_t rc = mares_puck_transfer (device, command, sizeof (command), answer, 2 * (len + 2));
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Extract the raw data from the packet.
		mares_puck_convert_ascii_to_binary (answer + 1, 2 * len, data, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_puck_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	if (! device_is_mares_puck (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < MARES_PUCK_MEMORY_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	device_status_t rc = mares_puck_device_read (abstract, 0x00, data, MARES_PUCK_MEMORY_SIZE);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (result)
		*result = MARES_PUCK_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}
