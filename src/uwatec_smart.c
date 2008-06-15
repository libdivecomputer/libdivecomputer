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
	unsigned int timestamp;
};


static void
uwatec_smart_discovery (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata)
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
	device->timestamp = 0;

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
	rc = irda_socket_discover (device->socket, uwatec_smart_discovery, device);
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
uwatec_smart_set_timestamp (smart *device, unsigned int timestamp)
{
	if (device == NULL)
		return UWATEC_ERROR;

	device->timestamp = timestamp;

	return UWATEC_SUCCESS;
}


static int
uwatec_smart_transfer (smart *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	int rc = irda_socket_write (device->socket, command, csize);
	if (rc != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	rc = irda_socket_read (device->socket, answer, asize);
	if (rc != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	return UWATEC_SUCCESS;
}


int
uwatec_smart_read (smart *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return UWATEC_ERROR;

	unsigned int timestamp = 0;
	unsigned char command[9] = {0};
	unsigned char answer[4] = {0};

	// Handshake (stage 1).

	command[0] = 0x1B;

	int rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != UWATEC_SUCCESS)
		return rc;

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

	rc = uwatec_smart_transfer (device, command, 5, answer, 1);
	if (rc != UWATEC_SUCCESS)
		return rc;

	message ("handshake: header=%02x\n", answer[0]);

	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return UWATEC_ERROR_PROTOCOL;
	}

	// Dive Computer Time.

	command[0] = 0x1A;

	rc = uwatec_smart_transfer (device, command, 1, answer, 4);
	if (rc != UWATEC_SUCCESS)
		return rc;

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

	rc = uwatec_smart_transfer (device, command, 1, answer, 4);
	if (rc != UWATEC_SUCCESS)
		return rc;

	unsigned int serial = answer[0] + (answer[1] << 8) + 
							(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: serial=0x%08x\n", serial);

	// Dive Computer Model.

	command[0] = 0x10;

	rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != UWATEC_SUCCESS)
		return rc;

	message ("handshake: model=0x%02x\n", answer[0]);

	// Data Length.

	command[0] = 0xC6;
	command[1] = (device->timestamp      ) & 0xFF;
	command[2] = (device->timestamp >> 8 ) & 0xFF;
	command[3] = (device->timestamp >> 16) & 0xFF;
	command[4] = (device->timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;

	rc = uwatec_smart_transfer (device, command, 9, answer, 4);
	if (rc != UWATEC_SUCCESS)
		return rc;

	unsigned int length = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: length=%u\n", length);

  	if (length == 0)
  		return 0;

	unsigned char *package = malloc (length * sizeof (unsigned char));
	if (package == NULL) {
		WARNING ("Memory allocation error.");
		return UWATEC_ERROR_MEMORY;
	}

	// Data.

	command[0] = 0xC4;
	command[1] = (device->timestamp      ) & 0xFF;
	command[2] = (device->timestamp >> 8 ) & 0xFF;
	command[3] = (device->timestamp >> 16) & 0xFF;
	command[4] = (device->timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;

	rc = uwatec_smart_transfer (device, command, 9, answer, 4);
	if (rc != UWATEC_SUCCESS) {
		free (package);
		return rc;
	}

	unsigned int total = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: total=%u\n", total);

	assert (total == length + 4);

	unsigned int nbytes = 0;
	while (nbytes < length) {
		unsigned int len = length - nbytes;
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

	if (length <= size) {
		memcpy (data, package, length);
	} else {
		WARNING ("Insufficient buffer space available.");
		return UWATEC_ERROR_MEMORY;
	}

	free (package);

	return length;
}


int
uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = data[current + 4] + (data[current + 5] << 8) + 
							(data[current + 6] << 16) + (data[current + 7] << 24);

			// Check for a buffer overflow.
			if (current + len > previous)
				return UWATEC_ERROR;

			if (callback)
				callback (data + current, len, userdata);

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return UWATEC_SUCCESS;
}
