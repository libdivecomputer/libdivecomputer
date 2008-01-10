#include <stdlib.h> // malloc, free

#include "uwatec.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


struct aladin {
	struct serial *port;
};


int
uwatec_aladin_open (aladin **out, const char* name)
{
	if (out == NULL)
		return UWATEC_ERROR;

	// Allocate memory.
	struct aladin *device = malloc (sizeof (struct aladin));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return UWATEC_ERROR_MEMORY;
	}

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Set the serial communication protocol (19200 8N1).
	rc = serial_configure (device->port, 19200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Set the timeout for receiving data (INFINITE).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Clear the RTS line and set the DTR line.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 0) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	*out = device;

	return UWATEC_SUCCESS;
}


int
uwatec_aladin_close (aladin *device)
{
	if (device == NULL)
		return UWATEC_SUCCESS;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Free memory.	
	free (device);

	return UWATEC_SUCCESS;
}


static void
uwatec_aladin_reverse (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size; ++i) {
		unsigned char j = 0;
		j  = (data[i] & 0x01) << 7;
		j += (data[i] & 0x02) << 5;
		j += (data[i] & 0x04) << 3;
		j += (data[i] & 0x08) << 1;
		j += (data[i] & 0x10) >> 1;
		j += (data[i] & 0x20) >> 3;
		j += (data[i] & 0x40) >> 5;
		j += (data[i] & 0x80) >> 7;
		data[i] = j;
	}
}


static unsigned short
uwatec_aladin_checksum (unsigned char data[], unsigned int size)
{
	unsigned short crc = 0x00;
	for (unsigned int i = 0; i < size; ++i)
		crc += data[i];

	return crc;
}


int
uwatec_aladin_read (aladin *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return UWATEC_ERROR;

	if (size < UWATEC_ALADIN_MEMORY_SIZE)
		return UWATEC_ERROR_MEMORY;

	// Receive the header of the package.
	for (unsigned int i = 0; i < 4;) {
		int rc = serial_read (device->port, data + i, 1);
		if (rc != 1) {
			WARNING ("Cannot read from device.");
			return UWATEC_ERROR;
		}
		if (data[i] == (i < 3 ? 0x55 : 0x00)) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Receive the contents of the package.
	int rc = serial_read (device->port, data + 4, 2046);
	if (rc != 2046) {
		WARNING ("Unexpected EOF in answer.");
		return UWATEC_ERROR;
	}

	// Reverse the bit order.
	uwatec_aladin_reverse (data, 2050);

	// Calculate the checksum.
	unsigned short ccrc = uwatec_aladin_checksum (data, 2048);

	// Verify the checksum of the package.
	unsigned short crc = (data[2049] << 8) + data[2048];
	if (ccrc != crc) {
		WARNING ("Unexpected answer CRC.");
		return UWATEC_ERROR;
	}

	return UWATEC_SUCCESS;
}
