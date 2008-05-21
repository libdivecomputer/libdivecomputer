#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "suunto.h"
#include "suunto_common.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? SUUNTO_ERROR_IO : SUUNTO_ERROR_TIMEOUT \
)

struct eon {
	struct serial *port;
};


int
suunto_eon_open (eon **out, const char* name)
{
	if (out == NULL)
		return SUUNTO_ERROR;

	// Allocate memory.
	struct eon *device = malloc (sizeof (struct eon));
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

	// Set the serial communication protocol (1200 8N2).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 2, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Clear the RTS line.
	if (serial_set_rts (device->port, 0)) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	*out = device;

	return SUUNTO_SUCCESS;
}


int
suunto_eon_close (eon *device)
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


unsigned char
suunto_eon_checksum (unsigned char data[], unsigned int size)
{
	unsigned char crc = 0x00;
	for (unsigned int i = 0; i < size; ++i)
		crc += data[i];

	return crc;
}


int
suunto_eon_read (eon *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// Send the command.
	unsigned char command[1] = {'P'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	// Receive the answer.
	unsigned char answer[SUUNTO_EON_MEMORY_SIZE + 1] = {0};
	rc = serial_read (device->port, answer, sizeof (answer));
	if (rc != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = suunto_eon_checksum (answer, sizeof (answer) - 1);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return SUUNTO_ERROR_PROTOCOL;
	}

	if (size >= SUUNTO_EON_MEMORY_SIZE) {
		memcpy (data, answer, SUUNTO_EON_MEMORY_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return SUUNTO_ERROR_MEMORY;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_eon_write_name (eon *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	if (size > 20)
		return SUUNTO_ERROR;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return SUUNTO_SUCCESS;
}


int
suunto_eon_write_interval (eon *device, unsigned char interval)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return SUUNTO_SUCCESS;
}


int
suunto_eon_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= SUUNTO_EON_MEMORY_SIZE);

	// Search the end-of-profile marker.
	unsigned int eop = 0x100;
	while (eop < SUUNTO_EON_MEMORY_SIZE) {
		if (data[eop] == 0x82) {
			break;
		}
		eop++;
	}

	return suunto_common_extract_dives (data, 0x100, SUUNTO_EON_MEMORY_SIZE, eop, 3, callback, userdata);
}
