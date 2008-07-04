#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "suunto.h"
#include "serial.h"
#include "utils.h"
#include "ringbuffer.h"

#define MAXRETRIES 2

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define RB_PROFILE_BEGIN			0x019A
#define RB_PROFILE_END				SUUNTO_D9_MEMORY_SIZE - 2
#define RB_PROFILE_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_PROFILE_BEGIN, RB_PROFILE_END)

struct d9 {
	struct serial *port;
};


int
suunto_d9_open (d9 **out, const char* name)
{
	if (out == NULL)
		return SUUNTO_ERROR;

	// Allocate memory.
	struct d9 *device = malloc (sizeof (struct d9));
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
suunto_d9_close (d9 *device)
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
suunto_d9_checksum (const unsigned char data[], unsigned int size, unsigned char init)
{
	unsigned char crc = init;
	for (unsigned int i = 0; i < size; ++i)
		crc ^= data[i];

	return crc;
}


static int
suunto_d9_send (d9 *device, const unsigned char command[], unsigned int csize)
{
	// Clear RTS to send the command.
	serial_set_rts (device->port, 0);

	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	// Receive the echo.
	unsigned char echo[128] = {0};
	assert (sizeof (echo) >= csize);
	int rc = serial_read (device->port, echo, csize);
	if (rc != csize) {
		WARNING ("Failed to receive the echo.");
		if (rc == -1)
			return SUUNTO_ERROR_IO;
		return SUUNTO_ERROR_TIMEOUT;
	}

	// Verify the echo.
	if (memcmp (command, echo, csize) != 0) {
		WARNING ("Unexpected echo.");
		return SUUNTO_ERROR_PROTOCOL;
	}

	// Set RTS to receive the reply.
	serial_set_rts (device->port, 1);

	return SUUNTO_SUCCESS;
}


static int
suunto_d9_transfer (d9 *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 4);

	// Occasionally, the dive computer does not respond to a command. 
	// In that case we retry the command a number of times before 
	// returning an error. Usually the dive computer will respond 
	// again during one of the retries.

	for (unsigned int i = 0;; ++i) {
		// Send the command to the dive computer.
		int rc = suunto_d9_send (device, command, csize);
		if (rc != SUUNTO_SUCCESS) {
			WARNING ("Failed to send the command.");
			return rc;
		}

		// Receive the answer of the dive computer.
		rc = serial_read (device->port, answer, asize);
		if (rc != asize) {
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return SUUNTO_ERROR_IO;
			if (i < MAXRETRIES)
				continue; // Retry.
			return SUUNTO_ERROR_TIMEOUT;
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
		unsigned char ccrc = suunto_d9_checksum (answer, asize - 1, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return SUUNTO_ERROR_PROTOCOL;
		}

		return SUUNTO_SUCCESS;
	}
}


int
suunto_d9_read_version (d9 *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	if (size < SUUNTO_D9_VERSION_SIZE)
		return SUUNTO_ERROR_MEMORY;

	unsigned char answer[SUUNTO_D9_VERSION_SIZE + 4] = {0};
	unsigned char command[4] = {0x0F, 0x00, 0x00, 0x0F};
	int rc = suunto_d9_transfer (device, command, sizeof (command), answer, sizeof (answer), 4);
	if (rc != SUUNTO_SUCCESS)
		return rc;

	memcpy (data, answer + 3, SUUNTO_D9_VERSION_SIZE);

#ifndef NDEBUG
	message ("D9ReadVersion()=\"%02x %02x %02x %02x\"\n", data[0], data[1], data[2], data[3]);
#endif

	return SUUNTO_SUCCESS;
}


int
suunto_d9_reset_maxdepth (d9 *device)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	unsigned char answer[4] = {0};
	unsigned char command[4] = {0x20, 0x00, 0x00, 0x20};
	int rc = suunto_d9_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != SUUNTO_SUCCESS)
		return rc;

#ifndef NDEBUG
	message ("D9ResetMaxDepth()\n");
#endif

	return SUUNTO_SUCCESS;
}


int
suunto_d9_read_memory (d9 *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_D9_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_D9_PACKET_SIZE);
		
		// Read the package.
		unsigned char answer[SUUNTO_D9_PACKET_SIZE + 7] = {0};
		unsigned char command[7] = {0x05, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[6] = suunto_d9_checksum (command, 6, 0x00);
		int rc = suunto_d9_transfer (device, command, sizeof (command), answer, len + 7, len);
		if (rc != SUUNTO_SUCCESS)
			return rc;

		memcpy (data, answer + 6, len);

#ifndef NDEBUG
		message ("D9Read(0x%04x,%d)=\"", address, len);
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
suunto_d9_write_memory (d9 *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_D9_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_D9_PACKET_SIZE);

		// Write the package.
		unsigned char answer[7] = {0};
		unsigned char command[SUUNTO_D9_PACKET_SIZE + 7] = {0x06, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (command + 6, data, len);
		command[len + 6] = suunto_d9_checksum (command, len + 6, 0x00);
		int rc = suunto_d9_transfer (device, command, len + 7, answer, sizeof (answer), 0);
		if (rc != SUUNTO_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("D9Write(0x%04x,%d,\"", address, len);
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


int
suunto_d9_read_dives (d9 *device, dive_callback_t callback, void *userdata)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// Read the header bytes.
	unsigned char header[8] = {0};
	int rc = suunto_d9_read_memory (device, 0x0190, header, sizeof (header));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory header.");
		return rc;
	}

	// Obtain the pointers from the header.
	unsigned int last  = header[0] + (header[1] << 8);
	unsigned int count = header[2] + (header[3] << 8);
	unsigned int end   = header[4] + (header[5] << 8);
	unsigned int begin = header[6] + (header[7] << 8);
	message ("Pointers: begin=%04x, last=%04x, end=%04x, count=%i\n", begin, last, end, count);

	// Memory buffer to store all the dives.

	unsigned char data[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	// Calculate the total amount of bytes.

	unsigned int remaining = RB_PROFILE_DISTANCE (begin, end);

	// To reduce the number of read operations, we always try to read 
	// packages with the largest possible size. As a consequence, the 
	// last package of a dive can contain data from more than one dive. 
	// Therefore, the remaining data of this package (and its size) 
	// needs to be preserved for the next dive.

	unsigned int available = 0;

	// The ring buffer is traversed backwards to retrieve the most recent
	// dives first. This allows you to download only the new dives. During 
	// the traversal, the current pointer does always point to the end of
	// the dive data and we move to the "next" dive by means of the previous 
	// pointer.

	unsigned int ndives = 0;
	unsigned int current = end;
	unsigned int previous = last;
	while (current != begin) {
		// Calculate the size of the current dive.
		unsigned int size = RB_PROFILE_DISTANCE (previous, current);
		message ("Pointers: dive=%u, current=%04x, previous=%04x, size=%u, remaining=%u, available=%u\n",
			ndives + 1, current, previous, size, remaining, available);

		assert (size >= 4 && size <= remaining);

		unsigned int nbytes = available;
		unsigned int address = current - available;
		while (nbytes < size) {
			// Calculate the package size. Try with the largest possible 
			// size first, and adjust when the end of the ringbuffer or  
			// the end of the profile data is reached.
			unsigned int len = SUUNTO_D9_PACKET_SIZE;
			if (RB_PROFILE_BEGIN + len > address)
				len = address - RB_PROFILE_BEGIN; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.
			/*if (nbytes + len > size)
				len = size - nbytes;*/ // End of dive (for testing only).

			message ("Pointers: address=%04x, len=%u\n", address - len, len);

			// Read the package.
			unsigned char *p = data + remaining - nbytes;
			rc = suunto_d9_read_memory (device, address - len, p - len, len);
			if (rc != SUUNTO_SUCCESS) {
				WARNING ("Cannot read memory.");
				return rc;
			}

			// Next package.
			nbytes += len;
			address -= len;
			if (address <= RB_PROFILE_BEGIN)
				address = RB_PROFILE_END;		
		}

		message ("Pointers: nbytes=%u\n", nbytes);

		// The last package of the current dive contains the previous and
		// next pointers (in a continuous memory area). It can also contain
		// a number of bytes from the next dive. The offset to the pointers
		// is equal to the number of bytes remaining after the current dive.

		remaining -= size;
		available = nbytes - size;

		unsigned int oprevious = data[remaining + 0] + (data[remaining + 1] << 8);
		unsigned int onext     = data[remaining + 2] + (data[remaining + 3] << 8);
		message ("Pointers: previous=%04x, next=%04x\n", oprevious, onext);
		assert (current == onext);

		// Next dive.
		current = previous;
		previous = oprevious;
		ndives++;

#ifndef NDEBUG
		message ("D9Profile()=\"");
		for (unsigned int i = 0; i < size - 4; ++i) {
			message ("%02x", data[remaining + 4 + i]);
		}
		message ("\"\n");
#endif

		if (callback)
			callback (data + remaining + 4, size - 4, userdata);
	}
	assert (remaining == 0);
	assert (available == 0);

	return SUUNTO_SUCCESS;
}
