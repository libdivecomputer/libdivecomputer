#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include "device-private.h"
#include "reefnet_sensuspro.h"
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


typedef struct reefnet_sensuspro_device_t reefnet_sensuspro_device_t;

struct reefnet_sensuspro_device_t {
	device_t base;
	struct serial *port;
};

static const device_backend_t reefnet_sensuspro_device_backend;

static int
device_is_reefnet_sensuspro (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensuspro_device_backend;
}


device_status_t
reefnet_sensuspro_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensuspro_device_t *device = malloc (sizeof (reefnet_sensuspro_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &reefnet_sensuspro_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (19200 8N1).
	rc = serial_configure (device->port, 19200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_device_close (device_t *abstract)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
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


static unsigned short
reefnet_sensuspro_checksum (unsigned char *data, unsigned int size)
{
	static unsigned short crc_ccitt_table[] = {
		0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
		0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
		0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
		0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
		0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
		0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
		0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
		0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
		0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
		0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
		0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12, 
		0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a, 
		0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41, 
		0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49, 
		0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
		0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78, 
		0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
		0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067, 
		0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
		0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
		0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
		0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
		0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
		0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
		0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
		0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
		0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
		0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
		0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
		0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
		0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
		0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
	};

	unsigned short crc = 0xffff;
	for (unsigned int i = 0; i < size; ++i)
		crc = (crc << 8) ^ crc_ccitt_table[(crc >> 8) ^ data[i]];

	return crc;
}


static device_status_t
reefnet_sensuspro_device_handshake (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Assert a break condition.
	serial_set_break (device->port, 1);

	// Receive the handshake from the dive computer.
	unsigned char handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE + 2] = {0};
	int rc = serial_read (device->port, handshake, sizeof (handshake));
	if (rc != sizeof (handshake)) {
		WARNING ("Failed to receive the handshake.");
		return EXITCODE (rc);
	}

	// Clear the break condition again.
	serial_set_break (device->port, 0);

	// Verify the checksum of the handshake packet.
	unsigned short crc = 
		 handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE + 0] + 
		(handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE + 1] << 8);
	unsigned short ccrc = reefnet_sensuspro_checksum (handshake, REEFNET_SENSUSPRO_HANDSHAKE_SIZE);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

#ifndef NDEBUG
	message (
		"Product Code:    %u\n"
		"Version Code:    %u\n"
		"Battery Voltage: %u\n"
		"Sample Interval: %u\n"
		"Device ID:       %u\n"
		"Current Time:    %u\n",
		handshake[0], handshake[1],
		handshake[2], handshake[3],
		handshake[4] + (handshake[5] << 8),
		handshake[6] + (handshake[7] << 8) + (handshake[8] << 16) + (handshake[9] << 24));
#endif

	if (size >= REEFNET_SENSUSPRO_HANDSHAKE_SIZE) {
		memcpy (data, handshake, REEFNET_SENSUSPRO_HANDSHAKE_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	serial_sleep (10);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensuspro_device_dump (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char command = 0xB4;
	int rc = serial_write (device->port, &command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	unsigned int nbytes = 0;
	unsigned char answer[REEFNET_SENSUSPRO_MEMORY_SIZE + 2] = {0};
	while (nbytes < sizeof (answer)) {
		unsigned int len = sizeof (answer) - nbytes;
		if (len > 256)
			len = 256;

		int rc = serial_read (device->port, answer + nbytes, len);
		if (rc != len) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (rc);
		}

		nbytes += len;
	}

	unsigned short crc = 
		 answer[REEFNET_SENSUSPRO_MEMORY_SIZE + 0] + 
		(answer[REEFNET_SENSUSPRO_MEMORY_SIZE + 1] << 8);
	unsigned short ccrc = reefnet_sensuspro_checksum (answer, REEFNET_SENSUSPRO_MEMORY_SIZE);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	if (size >= REEFNET_SENSUSPRO_MEMORY_SIZE) {
		memcpy (data, answer, REEFNET_SENSUSPRO_MEMORY_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return REEFNET_SENSUSPRO_MEMORY_SIZE;
}


device_status_t
reefnet_sensuspro_device_write_interval (device_t *abstract, unsigned char interval)
{
	reefnet_sensuspro_device_t *device = (reefnet_sensuspro_device_t*) abstract;

	if (! device_is_reefnet_sensuspro (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (interval < 1 || interval > 127)
		return DEVICE_STATUS_ERROR;

	unsigned char command = 0xB5;
	int rc = serial_write (device->port, &command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	serial_sleep (10);

	rc = serial_write (device->port, &interval, 1);
	if (rc != 1) {
		WARNING ("Failed to send the new value.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensuspro_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[2] = {0xFF, 0xFF};

	// Search the entire data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Once a start marker is found, start searching
			// for the corresponding stop marker. The search is 
			// now limited to the start of the previous dive.
			int found = 0;
			unsigned int offset = current + 10; // Skip non-sample data.
			while (offset + 2 <= previous) {
				if (memcmp (data + offset, footer, sizeof (footer)) == 0) {
					if (callback)
						callback (data + current, offset + 2 - current, userdata);

					found = 1;
					break;
				} else {
					offset++;
				}
			}

			// Report an error if no stop marker was found.
			if (!found)
				return DEVICE_STATUS_ERROR;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


static const device_backend_t reefnet_sensuspro_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUSPRO,
	reefnet_sensuspro_device_handshake, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensuspro_device_dump, /* dump */
	NULL, /* foreach */
	reefnet_sensuspro_device_close /* close */
};
