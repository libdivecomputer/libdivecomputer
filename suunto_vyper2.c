#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>	// assert

#include "suunto.h"
#include "serial.h"
#include "utils.h"

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc, n) \
( \
	rc == -1 ? \
	SUUNTO_ERROR_IO : \
	(rc != n ? SUUNTO_ERROR_TIMEOUT : SUUNTO_ERROR_PROTOCOL) \
)


struct vyper2 {
	struct serial *port;
};


int
suunto_vyper2_open (vyper2 **out, const char* name)
{
	if (out == NULL)
		return SUUNTO_ERROR;

	// Allocate memory.
	struct vyper2 *device = malloc (sizeof (struct vyper2));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return SUUNTO_ERROR_MEMORY;
	}

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = device;

	return SUUNTO_SUCCESS;
}


int
suunto_vyper2_close (vyper2 *device)
{
	if (device == NULL)
		return SUUNTO_SUCCESS;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Free memory.	
	free (device);

	return SUUNTO_SUCCESS;
}


static unsigned char
suunto_vyper2_checksum (const unsigned char data[], unsigned int size, unsigned char init)
{
	unsigned char crc = init;
	for (unsigned int i = 0; i < size; ++i)
		crc ^= data[i];

	return crc;
}


static int
suunto_vyper2_send (vyper2 *device, const unsigned char command[], unsigned int csize)
{
	serial_sleep (0x190 + 0xC8);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	serial_sleep (0x9);

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	return SUUNTO_SUCCESS;
}


static int
suunto_vyper2_recv (vyper2 *device, unsigned char data[], unsigned int size)
{
	int rc = serial_read (device->port, data, size);
	if (rc != size) {
		return EXITCODE (rc, size);
	}

	return SUUNTO_SUCCESS;
}


static int
suunto_vyper2_transfer (vyper2 *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 4);

	// Send the command to the dive computer.
	int rc = suunto_vyper2_send (device, command, csize);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	rc = suunto_vyper2_recv (device, answer, asize);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Failed to receive the answer.");
		return rc;
	}

	// Verify the header of the package.
	answer[2] -= size; // Adjust the package size for the comparision.
	if (memcmp (command, answer, asize - size - 1) != 0) {
		WARNING ("Unexpected answer start byte(s).");
		return SUUNTO_ERROR_PROTOCOL;
	}
	answer[2] += size; // Restore the package size again.

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = suunto_vyper2_checksum (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return SUUNTO_ERROR_PROTOCOL;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_vyper2_read_version (vyper2 *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	if (size < 4)
		return SUUNTO_ERROR_MEMORY;

	unsigned char answer[4 + 4] = {0};
	unsigned char command[4] = {0x0F, 0x00, 0x00, 0x0F};
	int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, sizeof (answer), 4);
	if (rc != SUUNTO_SUCCESS)
		return rc;

	memcpy (data, answer + 3, 4);

#ifndef NDEBUG
	message ("Vyper2ReadVersion()=\"%02x %02x %02x %02x\"\n", data[0], data[1], data[2], data[3]);
#endif

	return SUUNTO_SUCCESS;
}


int
suunto_vyper2_reset_maxdepth (vyper2 *device)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	unsigned char answer[4] = {0};
	unsigned char command[4] = {0x20, 0x00, 0x00, 0x20};
	int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != SUUNTO_SUCCESS)
		return rc;

#ifndef NDEBUG
	message ("Vyper2ResetMaxDepth()\n");
#endif

	return SUUNTO_SUCCESS;
}


int
suunto_vyper2_read_memory (vyper2 *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER2_PACKET_SIZE);
		
		// Read the package.
		unsigned char answer[SUUNTO_VYPER2_PACKET_SIZE + 7] = {0};
		unsigned char command[7] = {0x05, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[6] = suunto_vyper2_checksum (command, 6, 0x00);
		int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, len + 7, len);
		if (rc != SUUNTO_SUCCESS)
			return rc;

		memcpy (data, answer + 6, len);

#ifndef NDEBUG
		message ("Vyper2Read(0x%04x,%d)=\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_vyper2_write_memory (vyper2 *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER2_PACKET_SIZE);

		// Write the package.
		unsigned char answer[7] = {0};
		unsigned char command[SUUNTO_VYPER2_PACKET_SIZE + 7] = {0x06, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (command + 6, data, len);
		command[len + 6] = suunto_vyper2_checksum (command, len + 6, 0x00);
		int rc = suunto_vyper2_transfer (device, command, len + 7, answer, sizeof (answer), 0);
		if (rc != SUUNTO_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("Vyper2Write(0x%04x,%d,\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message ("%02x", data[i]);
		}
		message ("\");\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return SUUNTO_SUCCESS;
}
