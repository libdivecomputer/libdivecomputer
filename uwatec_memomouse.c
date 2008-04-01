#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include "uwatec.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc, n) \
( \
	rc == -1 ? \
	UWATEC_ERROR_IO : \
	(rc != n ? UWATEC_ERROR_TIMEOUT : UWATEC_ERROR_PROTOCOL) \
)

struct memomouse {
	struct serial *port;
};


static const unsigned char ack = 0x60, nak = 0xA8;


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
serial_read_uwatec (serial *device, unsigned char data[], unsigned int size)
{
	int rc = serial_read (device, data, size);

	if (rc > 0)
		uwatec_memomouse_reverse (data, rc);

	return rc;
}


static int
serial_write_uwatec (serial *device, const unsigned char data[], unsigned int size)
{
	int rc = serial_write (device, data, size);

	serial_drain (device);

	return rc;
}


static int
uwatec_memomouse_read_packet_outer (memomouse *device, unsigned char data[], unsigned int size)
{
	// Initial checksum value.
	unsigned char ccrc = 0x00;

	// Receive the header of the package.
	unsigned char len = 0x00;
	int rc = serial_read_uwatec (device->port, &len, 1);
	if (rc != 1 || len > size) {
		WARNING ("Unexpected answer start byte(s).");
		return EXITCODE (rc, 1);
	}
	// Update the checksum.
	ccrc = uwatec_memomouse_checksum (&len, 1, ccrc);

	// Receive the contents of the package.
	rc = serial_read_uwatec (device->port, data, len);
	if (rc != len) {
		WARNING ("Unexpected EOF in answer.");
		return EXITCODE (rc, len);
	}
	// Update the checksum.
	ccrc = uwatec_memomouse_checksum (data, len, ccrc);

	// Receive (and verify) the checksum of the package.
	unsigned char crc = 0x00;
	rc = serial_read_uwatec (device->port, &crc, 1);
	if (rc != 1 || ccrc != crc) {
		WARNING ("Unexpected answer CRC.");
		return EXITCODE (rc, 1);
	}

#ifndef NDEBUG
	message ("package(%i)=\"", len);
	for (unsigned int i = 0; i < len; ++i) {
		message ("%02x", data[i]);
	}
	message ("\"\n");
#endif

	return len;
}

static int
uwatec_memomouse_read_packet_inner (memomouse *device, unsigned char data[], unsigned int msize)
{
	// Initial checksum value.
	unsigned char ccrc = 0x00;

	// Read the first package.
	int rc = 0;
	unsigned char package[128 - 2] = {0};
	for (;;) {
		rc = uwatec_memomouse_read_packet_outer (device, package, sizeof (package));
		if (rc < 0) {
			message ("Didn't got expected packet, sending NAK\n");
			serial_flush (device->port, SERIAL_QUEUE_INPUT);
			serial_write_uwatec (device->port, &nak, 1);
			message ("Package: nbytes=%u, rc=%i\n", 0, rc);
		} else {
			message ("Got expected packet, sending ACK\n");
			serial_write_uwatec (device->port, &ack, 1);
			break;
		}
	}

	if (rc < 2) {
		message ("Package too small.\n");
		return UWATEC_ERROR;
	}

	// Calculate the total size of the inner package.
	unsigned int size = package[0] + (package[1] << 8) + 3;
	message ("Package: size=%u\n", size);

	// Calculate the size of the data in current package
	// (excluding the checksum).
	unsigned int len = (rc >= size ? size - 1 : rc);
	message ("Package: nbytes=%u, rc=%i, len=%u\n", 0, rc, len);

	// Update the checksum.
	ccrc = uwatec_memomouse_checksum (package, len, ccrc);

	// Append the data package to the output buffer.
	if (len - 2 <= msize)
		memcpy (data, package + 2, len - 2);
	else
		message ("Insufficient buffer space available.\n");

	unsigned int nbytes = rc;
	while (nbytes < size) {
		rc = uwatec_memomouse_read_packet_outer (device, package, sizeof (package));
		if (rc < 0) {
			message ("Didn't got expected packet, sending NAK\n");
			serial_flush (device->port, SERIAL_QUEUE_INPUT);
			serial_write_uwatec (device->port, &nak, 1);
			message ("Package: nbytes=%u, rc=%i\n", nbytes, rc);
		} else {
			message ("Got expected packet, sending ACK\n");
			serial_write_uwatec (device->port, &ack, 1);

			// Calculate the size of the data in current package
			// (excluding the checksum).
			len = (nbytes + rc >= size ? size - nbytes - 1 : rc);
			message ("Package: nbytes=%u, rc=%u, len=%u\n", nbytes, rc, len);

			// Update the checksum.
			ccrc = uwatec_memomouse_checksum (package, len, ccrc);

			// Append the data package to the output buffer.
			if (nbytes + len - 2 <= msize)
				memcpy (data + nbytes - 2, package, len);
			else
				message ("Insufficient buffer space available.\n");

			nbytes += rc;
		}
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
		message ("No greeting yet, sending NAK\n");
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
		serial_write_uwatec (device->port, &nak, 1);
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

	// Send upload command, until getting ACK.
	unsigned char answer = nak;
	while (answer != ack) {
		message ("Sending command\n");
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
		rc = serial_write (device->port, command, sizeof (command));
		if (rc != sizeof (command)) {
			WARNING ("Failed to send command");
			return EXITCODE (rc, sizeof (command));
		}

		rc = serial_read (device->port, &answer, 1);
		if (rc != 1) {
			WARNING ("Failed to recieve ACK");
			return EXITCODE (rc, 1);
		}

		if (answer != ack)
			message ("No ACK, got (0x%x)\n", answer);
		else
			message ("Received ACK\n");
	}

	// Wait for the transfer and read the data.
	message ("Waiting for transfer, access the log in your Aladin\n");
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
