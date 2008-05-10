#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr
#include <time.h>	// time, strftime
#include <assert.h>	// assert

#include "uwatec.h"
#include "irda.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? UWATEC_ERROR_IO : UWATEC_ERROR_TIMEOUT \
)

struct smart {
	struct irda *socket;
	unsigned int address;
};


void discovery (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata)
{
	message ("device: address=%08x, name=%s, charset=%02x, hints=%04x\n", address, name, charset, hints);

	smart *device = (smart *) userdata;
	if (device == NULL)
		return;

	if (strncmp (name, "UWATEC Galileo Sol", 18) == 0 ||
		strncmp (name, "Uwatec Smart", 12) == 0 ||
		strstr (name, "Uwatec") != NULL ||
		strstr (name, "UWATEC") != NULL ||
		strstr (name, "Aladin") != NULL ||
		strstr (name, "ALADIN") != NULL ||
		strstr (name, "Smart") != NULL ||
		strstr (name, "SMART") != NULL ||
		strstr (name, "Galileo") != NULL ||
		strstr (name, "GALILEO") != NULL) {
		message ("Found an Uwatec dive computer.\n");
		device->address = address;
	}
}


int
uwatec_smart_open (smart **out)
{
	if (out == NULL)
		return UWATEC_ERROR;

	// Allocate memory.
	struct smart *device = malloc (sizeof (struct smart));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return UWATEC_ERROR_MEMORY;
	}

	// Set the default values.
	device->socket = NULL;
	device->address = 0;

	irda_init ();

	// Open the irda socket.
	int rc = irda_socket_open (&device->socket);
	if (rc == -1) {
		WARNING ("Failed to open the irda socket.");
		irda_cleanup ();
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Discover the device.
	rc = irda_socket_discover (device->socket, discovery, device);
	if (rc == -1) {
		WARNING ("Failed to discover the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return UWATEC_ERROR_IO;
	}

	if (device->address == 0) {
		WARNING ("No dive computer found.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return UWATEC_ERROR;
	}

	// Connect the device.
	rc = irda_socket_connect_lsap (device->socket, device->address, 1);
	if (rc == -1) {
		WARNING ("Failed to connect the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return UWATEC_ERROR_IO;
	}

	*out = device;

	return UWATEC_SUCCESS;
}


int
uwatec_smart_close (smart *device)
{
	if (device == NULL)
		return UWATEC_SUCCESS;

	// Close the device.
	if (irda_socket_close (device->socket) == -1) {
		irda_cleanup ();
		free (device);
		return UWATEC_ERROR_IO;
	}

	irda_cleanup ();

	// Free memory.	
	free (device);

	return UWATEC_SUCCESS;
}


int
uwatec_smart_read (smart *device, unsigned char data[], unsigned int msize)
{
	if (device == NULL)
		return UWATEC_ERROR;

	unsigned int timestamp = 0;
	unsigned char command[9] = {0};
	unsigned char answer[4] = {0};

	// Handshake (stage 1).

	command[0] = 0x1B;
	int rc = irda_socket_write (device->socket, command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	message ("handshake: header=%02x\n", answer[0]);

	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return UWATEC_ERROR_PROTOCOL;
	}

	// Handshake (stage 2).

	command[0] = 0x1C;
	command[1] = 0x10;
	command[2] = 0x27;
	command[3] = 0;
	command[4] = 0;
	rc = irda_socket_write (device->socket, command, 5);
	if (rc != 5) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	message ("handshake: header=%02x\n", answer[0]);

	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return UWATEC_ERROR_PROTOCOL;
	}

	// Dive Computer Time.

	command[0] = 0x1A;
	rc = irda_socket_write (device->socket, command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 4);
	if (rc != 4) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	time_t device_time = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: timestamp=0x%08x\n", device_time);

	// PC Time and Time Correction.

	time_t now = time (NULL);
	unsigned char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("handshake: now=%lu (%s)\n", (unsigned long)now, datetime);

	// Serial Number

	command[0] = 0x14;
	rc = irda_socket_write (device->socket, command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 4);
	if (rc != 4) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	unsigned int serial = answer[0] + (answer[1] << 8) + 
							(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: serial=0x%08x\n", serial);

	// Dive Computer Model.

	command[0] = 0x10;
	rc = irda_socket_write (device->socket, command, 1);
	if (rc != 1) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	message ("handshake: model=0x%02x\n", answer[0]);

	// Data Length.

	command[0] = 0xC6;
	command[1] = (timestamp      ) & 0xFF;
	command[2] = (timestamp >> 8 ) & 0xFF;
	command[3] = (timestamp >> 16) & 0xFF;
	command[4] = (timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;
	rc = irda_socket_write (device->socket, command, 9);
	if (rc != 9) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 4);
	if (rc != 4) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	unsigned int size = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: size=%u\n", size);

  	if (size == 0)
  		return UWATEC_SUCCESS;

	unsigned char *package = malloc (size * sizeof (unsigned char));
	if (package == NULL) {
		WARNING ("Memory allocation error.");
		return UWATEC_ERROR_MEMORY;
	}

	// Data.

	command[0] = 0xC4;
	command[1] = (timestamp      ) & 0xFF;
	command[2] = (timestamp >> 8 ) & 0xFF;
	command[3] = (timestamp >> 16) & 0xFF;
	command[4] = (timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;
	rc = irda_socket_write (device->socket, command, 9);
	if (rc != 9) {
		WARNING ("Failed to send the command.");
		free (package);
		return EXITCODE (rc);
	}
	rc = irda_socket_read (device->socket, answer, 4);
	if (rc != 4) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	unsigned int length = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: size=%u\n", length);

	assert (length == size + 4);

	unsigned int nbytes = 0;
	while (nbytes < size) {
		unsigned int len = size - nbytes;
		if (len > 32)
			len = 32;
		rc = irda_socket_read (device->socket, package + nbytes, len);
		if (rc < 0) {
			WARNING ("Failed to receive the answer.");
			free (package);
			return EXITCODE (rc);
		}
		nbytes += rc;
		message ("len=%u, rc=%i, nbytes=%u\n", len, rc, nbytes);
	}

	if (size <= msize) {
		memcpy (data, package, size);
	} else {
		message ("Insufficient buffer space available.\n");
		memcpy (data, package, msize);
	}

	free (package);

	return UWATEC_SUCCESS;
}


int
uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	unsigned int offset = 0;
	while (offset + 8 <= size) {
		// Search for the header marker.
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = data[offset + 4] + (data[offset + 5] << 8) + 
							(data[offset + 6] << 16) + (data[offset + 7] << 24);

			// Check for a buffer overflow.
			if (offset + len > size)
				return UWATEC_ERROR;

			if (callback)
				callback (data + offset, len, userdata);

			offset += len;
		} else {
			offset++;
		}
	}

	return UWATEC_SUCCESS;
}
