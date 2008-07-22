#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "suunto_eon.h"
#include "suunto_common.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct suunto_eon_device_t suunto_eon_device_t;

struct suunto_eon_device_t {
	device_t base;
	struct serial *port;
};

static const device_backend_t suunto_eon_device_backend;

static int
device_is_suunto_eon (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_eon_device_backend;
}


device_status_t
suunto_eon_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_eon_device_t *device = malloc (sizeof (suunto_eon_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_eon_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (1200 8N2).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 2, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the RTS line.
	if (serial_set_rts (device->port, 0)) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_eon_device_close (device_t *abstract)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
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


static unsigned char
suunto_eon_checksum (unsigned char data[], unsigned int size)
{
	unsigned char crc = 0x00;
	for (unsigned int i = 0; i < size; ++i)
		crc += data[i];

	return crc;
}


static device_status_t
suunto_eon_device_dump (device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
		return DEVICE_STATUS_PROTOCOL;
	}

	if (size >= SUUNTO_EON_MEMORY_SIZE) {
		memcpy (data, answer, SUUNTO_EON_MEMORY_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return SUUNTO_EON_MEMORY_SIZE;
}


device_status_t
suunto_eon_device_write_name (device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size > 20)
		return DEVICE_STATUS_ERROR;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_eon_device_write_interval (device_t *abstract, unsigned char interval)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
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


static const device_backend_t suunto_eon_device_backend = {
	DEVICE_TYPE_SUUNTO_EON,
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	suunto_eon_device_dump, /* dump */
	NULL, /* foreach */
	suunto_eon_device_close /* close */
};
