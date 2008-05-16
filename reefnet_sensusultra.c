#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>

#include "reefnet.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? REEFNET_ERROR_IO : REEFNET_ERROR_TIMEOUT \
)

#define PROMPT 0xA5
#define ACCEPT PROMPT
#define REJECT 0x00

struct sensusultra {
	struct serial *port;
};


int
reefnet_sensusultra_open (sensusultra **out, const char* name)
{
	if (out == NULL)
		return REEFNET_ERROR;

	// Allocate memory.
	struct sensusultra *device = malloc (sizeof (struct sensusultra));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return REEFNET_ERROR_MEMORY;
	}

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return REEFNET_ERROR_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return REEFNET_ERROR_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return REEFNET_ERROR_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = device;

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_close (sensusultra *device)
{
	if (device == NULL)
		return REEFNET_SUCCESS;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return REEFNET_ERROR_IO;
	}

	// Free memory.	
	free (device);

	return REEFNET_SUCCESS;
}


static unsigned short
reefnet_sensusultra_checksum (const unsigned char *data, unsigned int size)
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


static int
reefnet_sensusultra_send_uchar (sensusultra *device, unsigned char value)
{
	// Wait for the prompt byte.
	unsigned char prompt = 0;
	int rc = serial_read (device->port, &prompt, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the prompt byte");
		return EXITCODE (rc);
	}

	// Verify the prompt byte.
	if (prompt != PROMPT) {
		WARNING ("Unexpected answer data.");
		return REEFNET_ERROR_PROTOCOL;
	}

	// Send the value to the device.
	rc = serial_write (device->port, &value, 1);
	if (rc != 1) {
		WARNING ("Failed to send the value.");
		return EXITCODE (rc);
	}

	return REEFNET_SUCCESS;
}


static int
reefnet_sensusultra_send_ushort (sensusultra *device, unsigned short value)
{
	// Send the least-significant byte.
	unsigned char lsb = value & 0xFF;
	int rc = reefnet_sensusultra_send_uchar (device, lsb);
	if (rc != REEFNET_SUCCESS)
		return rc;

	// Send the most-significant byte.
	unsigned char msb = (value >> 8) & 0xFF;
	rc = reefnet_sensusultra_send_uchar (device, msb);
	if (rc != REEFNET_SUCCESS)
		return rc;

	return REEFNET_SUCCESS;
}


static int
reefnet_sensusultra_packet (sensusultra *device, unsigned char *data, unsigned int size, unsigned int header)
{
	assert (size >= header + 2);

	// Receive the data packet.
	int rc = serial_read (device->port, data, size);
	if (rc != size) {
		WARNING ("Failed to receive the packet.");
		return EXITCODE (rc);
	}

	// Verify the checksum of the packet.
	unsigned short crc = data[size - 2] + (data[size - 1] << 8);
	unsigned short ccrc = reefnet_sensusultra_checksum (data + header, size - header - 2);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return REEFNET_ERROR_PROTOCOL;
	}

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_handshake (sensusultra *device, unsigned char *data, unsigned int size)
{
	if (device == NULL)
		return REEFNET_ERROR;

	// Flush the input and output buffers.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	int rc = 0;
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE + 2] = {0};
	while ((rc = reefnet_sensusultra_packet (device, handshake, sizeof (handshake), 0)) != REEFNET_SUCCESS) {
		// Automatically discard a corrupted handshake packet, 
		// and wait for the next one.
		if (rc != REEFNET_ERROR_PROTOCOL)
			return rc;

		// According to the developers guide, a 250 ms delay is suggested to
		// guarantee that the prompt byte sent after the handshake packet is 
		// not accidentally buffered by the host and (mis)interpreted as part 
		// of the next packet.

		serial_sleep (250);
		serial_flush (device->port, SERIAL_QUEUE_BOTH);
	}

#ifndef NDEBUG
	message (
		"Version:    %u\n"
		"Serial:     %u\n"
		"Time:       %u\n"
		"Boot Count: %u\n"
		"Boot Time:  %u\n"
		"Dive Count: %u\n"
		"Interval:   %u\n"
		"Threshold:  %u\n"
		"End Count:  %u\n"
		"Averaging:  %u\n",
		handshake[0] + (handshake[1] << 8),
		handshake[2] + (handshake[3] << 8),
		handshake[4] + (handshake[5] << 8) + (handshake[6] << 16) + (handshake[7] << 24),
		handshake[8] + (handshake[9] << 8),
		handshake[10] + (handshake[11] << 8) + (handshake[12] << 16) + (handshake[13] << 24),
		handshake[14] + (handshake[15] << 8),
		handshake[16] + (handshake[17] << 8),
		handshake[18] + (handshake[19] << 8),
		handshake[20] + (handshake[21] << 8),
		handshake[22] + (handshake[23] << 8));
#endif

	if (size >= REEFNET_SENSUSULTRA_HANDSHAKE_SIZE)
		memcpy (data, handshake, REEFNET_SENSUSULTRA_HANDSHAKE_SIZE);
	else
		WARNING ("Insufficient buffer space available.");

	return REEFNET_SUCCESS;
}


static int
reefnet_sensusultra_page (sensusultra *device, unsigned char *data, unsigned int size, unsigned int pagenum)
{
	if (device == NULL)
		return REEFNET_ERROR;

	int rc = 0;
	unsigned char package[REEFNET_SENSUSULTRA_PACKET_SIZE + 4] = {0};
	while ((rc = reefnet_sensusultra_packet (device, package, sizeof (package), 2)) != REEFNET_SUCCESS) {
		// Automatically discard a corrupted packet, 
		// and request a new one.
		if (rc != REEFNET_ERROR_PROTOCOL)
			return rc;

		// Reject the packet.
		rc = reefnet_sensusultra_send_uchar (device, REJECT);
		if (rc != REEFNET_SUCCESS)
			return rc;
	}

	// Verify the page number.
	unsigned int page = package[0] + (package[1] << 8);
	if (page != pagenum) {
		WARNING ("Unexpected page number."); 
		return REEFNET_ERROR_PROTOCOL;
	}

	if (size >= REEFNET_SENSUSULTRA_PACKET_SIZE)
		memcpy (data, package + 2, REEFNET_SENSUSULTRA_PACKET_SIZE);
	else
		WARNING ("Insufficient buffer space available.");

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_read_data (sensusultra *device, unsigned char *data, unsigned int size)
{
	if (device == NULL)
		return REEFNET_ERROR;

	if (size < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE)
		return REEFNET_ERROR;

	// Send the instruction code to the device.
	int rc = reefnet_sensusultra_send_ushort (device, 0xB421);
	if (rc != REEFNET_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE) {
		// Receive the packet.
		unsigned int offset = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE - 
			nbytes - REEFNET_SENSUSULTRA_PACKET_SIZE;
		rc = reefnet_sensusultra_page (device, data + offset, REEFNET_SENSUSULTRA_PACKET_SIZE, npages);
		if (rc != REEFNET_SUCCESS)
			return rc;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != REEFNET_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_read_user (sensusultra *device, unsigned char *data, unsigned int size)
{
	if (device == NULL)
		return REEFNET_ERROR;

	if (size < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE)
		return REEFNET_ERROR;

	// Send the instruction code to the device.
	int rc = reefnet_sensusultra_send_ushort (device, 0xB420);
	if (rc != REEFNET_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE) {
		// Receive the packet.
		rc = reefnet_sensusultra_page (device, data + nbytes, REEFNET_SENSUSULTRA_PACKET_SIZE, npages);
		if (rc != REEFNET_SUCCESS)
			return rc;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != REEFNET_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_write_user (sensusultra *device, const unsigned char *data, unsigned int size)
{
	if (device == NULL)
		return REEFNET_ERROR;

	assert (size >= REEFNET_SENSUSULTRA_MEMORY_USER_SIZE);

	// Send the instruction code to the device.
	int rc = reefnet_sensusultra_send_ushort (device, 0xB430);
	if (rc != REEFNET_SUCCESS)
		return rc;

	// Send the data to the device.
	for (unsigned int i = 0; i < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE; ++i) {
		rc = reefnet_sensusultra_send_uchar (device, data[i]);
		if (rc != REEFNET_SUCCESS)
			return rc;
	}

	// Send the checksum to the device.
	unsigned short crc = reefnet_sensusultra_checksum (data, REEFNET_SENSUSULTRA_MEMORY_USER_SIZE);
	rc = reefnet_sensusultra_send_ushort (device, crc);
	if (rc != REEFNET_SUCCESS)
		return rc;

	return REEFNET_SUCCESS;
}


static int
reefnet_sensusultra_write_internal (sensusultra *device, unsigned int code, unsigned int value)
{
	if (device == NULL)
		return REEFNET_ERROR;

	// Send the instruction code to the device.
	int rc = reefnet_sensusultra_send_ushort (device, code);
	if (rc != REEFNET_SUCCESS)
		return rc;

	// Send the new value to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != REEFNET_SUCCESS)
		return rc;

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_write_interval (sensusultra *device, unsigned int value)
{
	if (value < 1 || value > 65535)
		return REEFNET_ERROR;

	return reefnet_sensusultra_write_internal (device, 0xB410, value);
}


int
reefnet_sensusultra_write_threshold (sensusultra *device, unsigned int value)
{
	if (value < 1 || value > 65535)
		return REEFNET_ERROR;

	return reefnet_sensusultra_write_internal (device, 0xB411, value);
}


int
reefnet_sensusultra_write_endcount (sensusultra *device, unsigned int value)
{
	if (value < 1 || value > 65535)
		return REEFNET_ERROR;

	return reefnet_sensusultra_write_internal (device, 0xB412, value);
}


int
reefnet_sensusultra_write_averaging (sensusultra *device, unsigned int value)
{
	if (value != 1 && value != 2 && value != 4)
		return REEFNET_ERROR;

	return reefnet_sensusultra_write_internal (device, 0xB413, value);
}


int
reefnet_sensusultra_sense (sensusultra *device, unsigned char *data, unsigned int size)
{
	if (device == NULL)
		return REEFNET_ERROR;

	// Send the instruction code to the device.
	int rc = reefnet_sensusultra_send_ushort (device, 0xB440);
	if (rc != REEFNET_SUCCESS)
		return rc;

	// Receive the packet.
	unsigned char package[REEFNET_SENSUSULTRA_SENSE_SIZE + 2] = {0};
	rc = reefnet_sensusultra_packet (device, package, sizeof (package), 0);
	if (rc != REEFNET_SUCCESS)
		return rc;

	if (size >= REEFNET_SENSUSULTRA_SENSE_SIZE)
		memcpy (data, package, REEFNET_SENSUSULTRA_SENSE_SIZE);
	else
		WARNING ("Insufficient buffer space available.");

	return REEFNET_SUCCESS;
}


int
reefnet_sensusultra_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

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
			unsigned int offset = current + 16; // Skip non-sample data.
			while (offset + 4 <= previous) {
				if (memcmp (data + offset, footer, sizeof (footer)) == 0) {
					if (callback)
						callback (data + current, offset + 4 - current, userdata);

					found = 1;
					break;
				} else {
					offset++;
				}
			}

			// Report an error if no stop marker was found.
			if (!found)
				return REEFNET_ERROR;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return REEFNET_SUCCESS;
}
