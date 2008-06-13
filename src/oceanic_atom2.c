#include <string.h> // memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "oceanic.h"
#include "serial.h"
#include "utils.h"

#define MAXRETRIES 2

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


struct atom2 {
	struct serial *port;
};


static unsigned char
oceanic_atom2_checksum (const unsigned char data[], unsigned int size, unsigned char init)
{
	unsigned char crc = init;
	for (unsigned int i = 0; i < size; ++i)
		crc += data[i];

	return crc;
}


static int
oceanic_atom2_send (atom2 *device, const unsigned char command[], unsigned int csize)
{
	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	return OCEANIC_SUCCESS;
}


static int
oceanic_atom2_transfer (atom2 *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, int handshake)
{
	assert (asize >= 2);

	// Occasionally, the dive computer does not respond to a command. 
	// In that case we retry the command a number of times before 
	// returning an error. Usually the dive computer will respond 
	// again during one of the retries.

	for (unsigned int i = 0;; ++i) {
		// Send the command to the dive computer.
		int rc = oceanic_atom2_send (device, command, csize);
		if (rc != OCEANIC_SUCCESS) {
			WARNING ("Failed to send the command.");
			return rc;
		}

		// Receive the answer of the dive computer.
		rc = serial_read (device->port, answer, asize);
		if (rc != asize) {
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return OCEANIC_ERROR_IO;
			if (i < MAXRETRIES)
				continue; // Retry.
			return OCEANIC_ERROR_TIMEOUT;
		}

		// Verify the header of the package.
		unsigned char header = (handshake ? 0xA5 : 0x5A);
		if (answer[0] != header) {
			WARNING ("Unexpected answer start byte(s).");
			return OCEANIC_ERROR_PROTOCOL;
		}

		// Verify the checksum of the package.
		unsigned char crc = answer[asize - 1];
		unsigned char ccrc = oceanic_atom2_checksum (answer + 1, asize - 2, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return OCEANIC_ERROR_PROTOCOL;
		}

		return OCEANIC_SUCCESS;
	}
}


int
oceanic_atom2_open (atom2 **out, const char* name)
{
	if (out == NULL)
		return OCEANIC_ERROR;

	// Allocate memory.
	struct atom2 *device = malloc (sizeof (struct atom2));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return OCEANIC_ERROR_MEMORY;
	}

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return OCEANIC_ERROR_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return OCEANIC_ERROR_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return OCEANIC_ERROR_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Send the handshake to connect to the device.
	unsigned char answer[3] = {0};
	unsigned char command[3] = {0xA8, 0x99, 0x00};
	rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
	if (rc != OCEANIC_SUCCESS) {
		serial_close (device->port);
		free (device);
		return rc;
	}

	// Verify the handshake.
	if (answer[1] != 0xA5) {
		WARNING ("Unexpected handshake byte(s).");
		serial_close (device->port);
		free (device);
		return OCEANIC_ERROR_PROTOCOL;
	}

	*out = device;

	return OCEANIC_SUCCESS;
}


int
oceanic_atom2_close (atom2 *device)
{
	if (device == NULL)
		return OCEANIC_SUCCESS;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return OCEANIC_ERROR_IO;
	}

	// Free memory.	
	free (device);

	return OCEANIC_SUCCESS;
}


int
oceanic_atom2_read_version (atom2 *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return OCEANIC_ERROR;

	if (size < OCEANIC_ATOM2_PACKET_SIZE)
		return OCEANIC_ERROR_MEMORY;

	unsigned char answer[OCEANIC_ATOM2_PACKET_SIZE + 2] = {0};
	unsigned char command[2] = {0x84, 0x00};
	int rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != OCEANIC_SUCCESS)
		return rc;

	memcpy (data, answer + 1, OCEANIC_ATOM2_PACKET_SIZE);

#ifndef NDEBUG
	answer[OCEANIC_ATOM2_PACKET_SIZE + 1] = 0;
	message ("ATOM2ReadVersion()=\"%s\"\n", answer + 1);
#endif

	return OCEANIC_SUCCESS;
}


int
oceanic_atom2_read_memory (atom2 *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return OCEANIC_ERROR;

	assert (address % OCEANIC_ATOM2_PACKET_SIZE == 0);
	assert (size    % OCEANIC_ATOM2_PACKET_SIZE == 0);
	
	// The data transmission is split in packages
	// of maximum $OCEANIC_ATOM2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / OCEANIC_ATOM2_PACKET_SIZE;
		unsigned char answer[OCEANIC_ATOM2_PACKET_SIZE + 2] = {0};
		unsigned char command[4] = {0xB1, 
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0};
		int rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
		if (rc != OCEANIC_SUCCESS)
			return rc;

		memcpy (data, answer + 1, OCEANIC_ATOM2_PACKET_SIZE);

#ifndef NDEBUG
		message ("ATOM2Read(0x%04x,%d)=\"", address, OCEANIC_ATOM2_PACKET_SIZE);
		for (unsigned int i = 0; i < OCEANIC_ATOM2_PACKET_SIZE; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		nbytes += OCEANIC_ATOM2_PACKET_SIZE;
		address += OCEANIC_ATOM2_PACKET_SIZE;
		data += OCEANIC_ATOM2_PACKET_SIZE;
	}

	return OCEANIC_SUCCESS;
}
