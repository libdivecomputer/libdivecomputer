#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "uwatec.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? UWATEC_ERROR_IO : UWATEC_ERROR_TIMEOUT \
)

#define ACK 0x60
#define NAK 0xA8

struct memomouse {
	struct serial *port;
};


int
uwatec_memomouse_open (memomouse **out, const char* name)
{
	if (out == NULL)
		return UWATEC_ERROR;

	// Allocate memory.
	struct memomouse *device = malloc (sizeof (struct memomouse));
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

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Set the timeout for receiving data (60s).
	if (serial_set_timeout (device->port, 60000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	serial_sleep (200);

	serial_flush (device->port, SERIAL_QUEUE_BOTH);

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
uwatec_memomouse_close (memomouse *device)
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
uwatec_memomouse_reverse (unsigned char data[], unsigned int size)
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


static unsigned char
uwatec_memomouse_checksum (unsigned char data[], unsigned int size, unsigned char init)
{
	unsigned char crc = init;
	for (unsigned int i = 0; i < size; ++i)
		crc ^= data[i];

	return crc;
}


static int
uwatec_memomouse_confirm (memomouse *device, unsigned char value)
{
	// Send the value to the device.
	int rc = serial_write (device->port, &value, 1);
	if (rc != 1) {
		WARNING ("Failed to send the value.");
		return EXITCODE (rc);
	}

	serial_drain (device->port);

	return UWATEC_SUCCESS;
}


static int
uwatec_memomouse_read_packet (memomouse *device, unsigned char data[], unsigned int size)
{
	assert (size >= 126 + 2);

	// Receive the header of the package.
	int rc = serial_read (device->port, data, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Reverse the bits.
	uwatec_memomouse_reverse (data, 1);

	// Verify the header of the package.
	unsigned int len = data[0];
	if (len > 126) {
		WARNING ("Unexpected answer start byte(s).");
		return UWATEC_ERROR_PROTOCOL;
	}

	// Receive the remaining part of the package.
	rc = serial_read (device->port, data + 1, len + 1);
	if (rc != len + 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Reverse the bits.
	uwatec_memomouse_reverse (data + 1, len + 1);

	// Verify the checksum of the package.
	unsigned char crc = data[len + 1];
	unsigned char ccrc = uwatec_memomouse_checksum (data, len + 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return UWATEC_ERROR_PROTOCOL;
	}

	return len;
}


static int
uwatec_memomouse_read_packet_outer (memomouse *device, unsigned char data[], unsigned int size)
{
	int rc = 0;
	unsigned char package[126 + 2] = {0};
	while ((rc = uwatec_memomouse_read_packet (device, package, sizeof (package))) < 0) {
		// Automatically discard a corrupted packet, 
		// and request a new one.
		if (rc != UWATEC_ERROR_PROTOCOL)
			return rc;	

		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);

		// Reject the packet.
		rc = uwatec_memomouse_confirm (device, NAK);
		if (rc != UWATEC_SUCCESS)
			return rc;
	}

#ifndef NDEBUG
	message ("package(%i)=\"", rc);
	for (unsigned int i = 0; i < rc; ++i) {
		message ("%02x", package[i + 1]);
	}
	message ("\"\n");
#endif

	if (size >= rc)
		memcpy (data, package + 1, rc);
	else
		WARNING ("Insufficient buffer space available.");

	return rc;
}


static int
uwatec_memomouse_read_packet_inner (memomouse *device, unsigned char data[], unsigned int size)
{
	// Initial checksum value.
	unsigned char ccrc = 0x00;

	// Read the first package.
	unsigned char package[126] = {0};
	int rca = uwatec_memomouse_read_packet_outer (device, package, sizeof (package));
	if (rca < 0)
		return rca;

	// Accept the package.
	int rcb = uwatec_memomouse_confirm (device, ACK);
	if (rcb != UWATEC_SUCCESS)
		return rcb;

	if (rca < 2) {
		message ("First package is too small.\n");
		return UWATEC_ERROR_PROTOCOL;
	}

	// Calculate the total size of the inner package.
	unsigned int total = package[0] + (package[1] << 8) + 3;
	message ("Package: size=%u\n", total);

	// Calculate the size of the data in current package
	// (excluding the checksum).
	unsigned int len = (rca >= total ? total - 1 : rca);
	message ("Package: nbytes=%u, rc=%i, len=%u\n", 0, rca, len);

	// Update the checksum.
	ccrc = uwatec_memomouse_checksum (package, len, ccrc);

	// Append the data package to the output buffer.
	if (len - 2 <= size)
		memcpy (data, package + 2, len - 2);
	else
		message ("Insufficient buffer space available.\n");

	unsigned int nbytes = rca;
	while (nbytes < total) {
		// Read the package.
		rca = uwatec_memomouse_read_packet_outer (device, package, sizeof (package));
		if (rca < 0)
			return rca;

		// Accept the package.
		rcb = uwatec_memomouse_confirm (device, ACK);
		if (rcb != UWATEC_SUCCESS)
			return rcb;

		// Calculate the size of the data in current package
		// (excluding the checksum).
		len = (nbytes + rca >= total ? total - nbytes - 1 : rca);
		message ("Package: nbytes=%u, rc=%u, len=%u\n", nbytes, rca, len);

		// Update the checksum.
		ccrc = uwatec_memomouse_checksum (package, len, ccrc);

		// Append the data package to the output buffer.
		if (nbytes + len - 2 <= size)
			memcpy (data + nbytes - 2, package, len);
		else
			message ("Insufficient buffer space available.\n");

		nbytes += rca;
	}

	message ("Package: nbytes=%i\n", nbytes);

	// Verify the checksum.
	unsigned char crc = package[len];
	if (crc != ccrc)
		return UWATEC_ERROR;

	return UWATEC_SUCCESS;
}


int
uwatec_memomouse_read (memomouse *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return UWATEC_ERROR;

	// Waiting for greeting message.
	while (serial_get_received (device->port) == 0) {
		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);

		// Reject the packet.
		int rc = uwatec_memomouse_confirm (device, NAK);
		if (rc != UWATEC_SUCCESS)
			return rc;

		serial_sleep (300);
	}

	// Read the ID string.
	unsigned char id[7] = {0};
	int rc = uwatec_memomouse_read_packet_inner (device, id, sizeof (id));
	if (rc != UWATEC_SUCCESS)
		return UWATEC_ERROR;

	// Prepare the command.
	unsigned char command [9] = {
		0x07, 					// Outer packet size.
		0x05, 0x00, 			// Inner packet size.
		0x55, 					// Command byte.
		0x00, 0x00, 0x00, 0x00, // Timestamp.
		0x00}; 					// Outer packet checksum.
	command[8] = uwatec_memomouse_checksum (command, 8, 0x00);
	uwatec_memomouse_reverse (command, sizeof (command));

	// Keep send the command to the device, 
	// until the ACK answer is received.
	unsigned char answer = NAK;
	while (answer != ACK) {
		// Flush the input buffer.
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
		
		// Send the command to the device.
		rc = serial_write (device->port, command, sizeof (command));
		if (rc != sizeof (command)) {
			WARNING ("Failed to send the command.");
			return EXITCODE (rc);
		}

		serial_drain (device->port);

		// Wait for the answer (ACK).
		rc = serial_read (device->port, &answer, 1);
		if (rc != 1) {
			WARNING ("Failed to recieve the answer.");
			return EXITCODE (rc);
		}
	}

	// Wait for the transfer and read the data.
	rc = uwatec_memomouse_read_packet_inner (device, data, size);
	if (rc != UWATEC_SUCCESS)
		return UWATEC_ERROR;

	return UWATEC_SUCCESS;
}


int
uwatec_memomouse_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	// Parse the data stream to find the total number of dives.
	unsigned int ndives = 0;
	unsigned int previous = 0;
	unsigned int current = 5;
	while (current + 18 <= size) {
		// Memomouse sends all the data twice. The first time, it sends 
		// the data starting from the oldest dive towards the newest dive. 
		// Next, it send the same data in reverse order (newest to oldest).
		// We abort the parsing once we detect the first duplicate dive.
		// The second data stream contains always exactly 37 dives, and not
		// all dives have profile data, so it's probably data from the
		// connected Uwatec Aladin (converted to the memomouse format).
		if (previous && memcmp (data + previous, data + current, 18) == 0)
			break;

		// Get the length of the profile data.
		unsigned int len = data[current + 16] + (data[current + 17] << 8);

		// Check for a buffer overflow.
		if (current + len + 18 > size)
			return UWATEC_ERROR;

		// Move to the next dive.
		previous = current;
		current += len + 18;
		ndives++;
	}

	// Parse the data stream again to return each dive in reverse order
	// (newest dive first). This is less efficient, since the data stream
	// needs to be scanned multiple times, but it makes the behaviour
	// consistent with the equivalent function for the Uwatec Aladin.
	for (unsigned int i = 0; i < ndives; ++i) {
		// Skip the older dives.
		unsigned int offset = 5;
		unsigned int skip = ndives - i - 1;
		while (skip) {
			// Get the length of the profile data.
			unsigned int len = data[offset + 16] + (data[offset + 17] << 8);
			// Move to the next dive.
			offset += len + 18;
			skip--;
		}

		// Get the length of the profile data.
		unsigned int length = data[offset + 16] + (data[offset + 17] << 8);

		if (callback)
			callback (data + offset, length + 18, userdata);
	}

	return UWATEC_SUCCESS;
}
