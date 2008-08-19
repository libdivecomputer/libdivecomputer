#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "suunto_vyper2.h"
#include "serial.h"
#include "utils.h"
#include "ringbuffer.h"
#include "checksum.h"

#define MAXRETRIES 2

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define RB_PROFILE_BEGIN			0x019A
#define RB_PROFILE_END				SUUNTO_VYPER2_MEMORY_SIZE - 2
#define RB_PROFILE_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_PROFILE_BEGIN, RB_PROFILE_END)


typedef struct suunto_vyper2_device_t suunto_vyper2_device_t;

struct suunto_vyper2_device_t {
	device_t base;
	struct serial *port;
};

static const device_backend_t suunto_vyper2_device_backend;

static int
device_is_suunto_vyper2 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_vyper2_device_backend;
}


device_status_t
suunto_vyper2_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_vyper2_device_t *device = malloc (sizeof (suunto_vyper2_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_vyper2_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_device_close (device_t *abstract)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
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


static device_status_t
suunto_vyper2_send (suunto_vyper2_device_t *device, const unsigned char command[], unsigned int csize)
{
	serial_sleep (0x190 + 0xC8);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	serial_sleep (0x9);

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_transfer (suunto_vyper2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 4);

	// Occasionally, the dive computer does not respond to a command. 
	// In that case we retry the command a number of times before 
	// returning an error. Usually the dive computer will respond 
	// again during one of the retries.

	for (unsigned int i = 0;; ++i) {
		// Send the command to the dive computer.
		int rc = suunto_vyper2_send (device, command, csize);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Failed to send the command.");
			return rc;
		}

		// Receive the answer of the dive computer.
		rc = serial_read (device->port, answer, asize);
		if (rc != asize) {
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return DEVICE_STATUS_IO;
			if (i < MAXRETRIES)
				continue; // Retry.
			return DEVICE_STATUS_TIMEOUT;
		}

		// Verify the header of the package.
		answer[2] -= size; // Adjust the package size for the comparision.
		if (memcmp (command, answer, asize - size - 1) != 0) {
			WARNING ("Unexpected answer start byte(s).");
			return DEVICE_STATUS_PROTOCOL;
		}
		answer[2] += size; // Restore the package size again.

		// Verify the checksum of the package.
		unsigned char crc = answer[asize - 1];
		unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		return DEVICE_STATUS_SUCCESS;
	}
}


static device_status_t
suunto_vyper2_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_VYPER2_VERSION_SIZE)
		return DEVICE_STATUS_MEMORY;

	unsigned char answer[SUUNTO_VYPER2_VERSION_SIZE + 4] = {0};
	unsigned char command[4] = {0x0F, 0x00, 0x00, 0x0F};
	int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, sizeof (answer), 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer + 3, SUUNTO_VYPER2_VERSION_SIZE);

#ifndef NDEBUG
	message ("Vyper2ReadVersion()=\"%02x %02x %02x %02x\"\n", data[0], data[1], data[2], data[3]);
#endif

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper2_device_reset_maxdepth (device_t *abstract)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char answer[4] = {0};
	unsigned char command[4] = {0x20, 0x00, 0x00, 0x20};
	int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

#ifndef NDEBUG
	message ("Vyper2ResetMaxDepth()\n");
#endif

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size, device_progress_state_t *progress)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER2_PACKET_SIZE);
		
		// Read the package.
		unsigned char answer[SUUNTO_VYPER2_PACKET_SIZE + 7] = {0};
		unsigned char command[7] = {0x05, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[6] = checksum_xor_uint8 (command, 6, 0x00);
		int rc = suunto_vyper2_transfer (device, command, sizeof (command), answer, len + 7, len);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 6, len);

#ifndef NDEBUG
		message ("Vyper2Read(0x%04x,%d)=\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		progress_event (progress, DEVICE_EVENT_PROGRESS, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	return suunto_vyper2_read (abstract, address, data, size, NULL);
}


static device_status_t
suunto_vyper2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	suunto_vyper2_device_t *device = (suunto_vyper2_device_t*) abstract;

	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER2_PACKET_SIZE);

		// Write the package.
		unsigned char answer[7] = {0};
		unsigned char command[SUUNTO_VYPER2_PACKET_SIZE + 7] = {0x06, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (command + 6, data, len);
		command[len + 6] = checksum_xor_uint8 (command, len + 6, 0x00);
		int rc = suunto_vyper2_transfer (device, command, len + 7, answer, sizeof (answer), 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("Vyper2Write(0x%04x,%d,\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message ("%02x", data[i]);
		}
		message ("\");\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper2_device_dump (device_t *abstract, unsigned char data[], unsigned int size)
{
	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_VYPER2_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, SUUNTO_VYPER2_MEMORY_SIZE);

	int rc = suunto_vyper2_read (abstract, 0x00, data, SUUNTO_VYPER2_MEMORY_SIZE, &progress);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return SUUNTO_VYPER2_MEMORY_SIZE;
}


static device_status_t
suunto_vyper2_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_suunto_vyper2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, RB_PROFILE_END - RB_PROFILE_BEGIN + 8);

	// Read the header bytes.
	unsigned char header[8] = {0};
	int rc = suunto_vyper2_read (abstract, 0x0190, header, sizeof (header), NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
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

	progress_set_maximum (&progress, remaining + 8);
	progress_event (&progress, DEVICE_EVENT_PROGRESS, 8);

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
			unsigned int len = SUUNTO_VYPER2_PACKET_SIZE;
			if (RB_PROFILE_BEGIN + len > address)
				len = address - RB_PROFILE_BEGIN; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.
			/*if (nbytes + len > size)
				len = size - nbytes;*/ // End of dive (for testing only).

			message ("Pointers: address=%04x, len=%u\n", address - len, len);

			// Read the package.
			unsigned char *p = data + remaining - nbytes;
			rc = suunto_vyper2_read (abstract, address - len, p - len, len, &progress);
			if (rc != DEVICE_STATUS_SUCCESS) {
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
		message ("Vyper2Profile()=\"");
		for (unsigned int i = 0; i < size - 4; ++i) {
			message ("%02x", data[remaining + 4 + i]);
		}
		message ("\"\n");
#endif

		if (callback && !callback (data + remaining + 4, size - 4, userdata))
			return DEVICE_STATUS_SUCCESS;
	}
	assert (remaining == 0);
	assert (available == 0);

	return DEVICE_STATUS_SUCCESS;
}


static const device_backend_t suunto_vyper2_device_backend = {
	DEVICE_TYPE_SUUNTO_VYPER2,
	NULL, /* handshake */
	suunto_vyper2_device_version, /* version */
	suunto_vyper2_device_read, /* read */
	suunto_vyper2_device_write, /* write */
	suunto_vyper2_device_dump, /* dump */
	suunto_vyper2_device_foreach, /* foreach */
	suunto_vyper2_device_close /* close */
};
