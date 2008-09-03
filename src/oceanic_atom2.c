#include <string.h> // memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "oceanic_atom2.h"
#include "serial.h"
#include "utils.h"
#include "ringbuffer.h"
#include "checksum.h"

#define MAXRETRIES 2

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define ACK 0x5A
#define NAK 0xA5

#define RB_LOGBOOK_EMPTY			0x0230
#define RB_LOGBOOK_BEGIN			0x0240
#define RB_LOGBOOK_END				0x0A40
#define RB_LOGBOOK_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END)

#define RB_PROFILE_EMPTY			0x0A40
#define RB_PROFILE_BEGIN			0x0A50
#define RB_PROFILE_END				0xFFF0
#define RB_PROFILE_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define PT_LOGBOOK_FIRST(x)			( (x)[4] + ((x)[5] << 8) )
#define PT_LOGBOOK_LAST(x)			( (x)[6] + ((x)[7] << 8) )

#define PT_PROFILE_FIRST(x)			( (x)[5] + (((x)[6] & 0x0F) << 8) )
#define PT_PROFILE_LAST(x)			( ((x)[6] >> 4) + ((x)[7] << 4) )


typedef struct oceanic_atom2_device_t oceanic_atom2_device_t;

struct oceanic_atom2_device_t {
	device_t base;
	struct serial *port;
};

static device_status_t oceanic_atom2_device_version (device_t *abstract, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t oceanic_atom2_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t oceanic_atom2_device_close (device_t *abstract);

static const device_backend_t oceanic_atom2_device_backend = {
	DEVICE_TYPE_OCEANIC_ATOM2,
	NULL, /* handshake */
	oceanic_atom2_device_version, /* version */
	oceanic_atom2_device_read, /* read */
	NULL, /* write */
	oceanic_atom2_device_dump, /* dump */
	oceanic_atom2_device_foreach, /* foreach */
	oceanic_atom2_device_close /* close */
};

static int
device_is_oceanic_atom2 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_atom2_device_backend;
}


static device_status_t
oceanic_atom2_send (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize)
{
	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	unsigned char response = NAK;
	while (response == NAK) {
		// Send the command to the dive computer.
		device_status_t rc = oceanic_atom2_send (device, command, csize);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Failed to send the command.");
			return rc;
		}

		// Receive the response (ACK/NAK) of the dive computer.
		int n = serial_read (device->port, &response, 1);
		if (n != 1) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

#ifndef NDEBUG
		if (response != ACK)
			message ("Received unexpected response (%02x).\n", response);
#endif

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			break;
	}

	// Verify the response of the dive computer.
	if (response != ACK) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	if (asize) {
		// Receive the answer of the dive computer.
		int rc = serial_read (device->port, answer, asize);
		if (rc != asize) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (rc);
		}

		// Verify the checksum of the answer.
		unsigned char crc = answer[asize - 1];
		unsigned char ccrc = checksum_add_uint8 (answer, asize - 1, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
oceanic_atom2_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t *) malloc (sizeof (oceanic_atom2_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &oceanic_atom2_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
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

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_close (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
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


device_status_t
oceanic_atom2_device_handshake (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[3] = {0xA8, 0x99, 0x00};
	device_status_t rc = oceanic_atom2_send (device, command, sizeof (command));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	unsigned char answer[3] = {0};
	int n = serial_read (device->port, answer, sizeof (answer));
	if (n != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the answer.
	if (answer[0] != NAK || answer[1] != NAK || answer[2] != NAK) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
oceanic_atom2_device_quit (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[4] = {0x6A, 0x05, 0xA5, 0x00};
	device_status_t rc = oceanic_atom2_send (device, command, sizeof (command));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	unsigned char answer[1] = {0};
	int n = serial_read (device->port, answer, sizeof (answer));
	if (n != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the answer.
	if (answer[0] != 0xA5) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < OCEANIC_ATOM2_PACKET_SIZE)
		return DEVICE_STATUS_MEMORY;

	unsigned char answer[OCEANIC_ATOM2_PACKET_SIZE + 1] = {0};
	unsigned char command[2] = {0x84, 0x00};
	device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer, OCEANIC_ATOM2_PACKET_SIZE);

#ifndef NDEBUG
	answer[OCEANIC_ATOM2_PACKET_SIZE] = 0;
	message ("ATOM2ReadVersion()=\"%s\"\n", answer);
#endif

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	assert (address % OCEANIC_ATOM2_PACKET_SIZE == 0);
	assert (size    % OCEANIC_ATOM2_PACKET_SIZE == 0);
	
	// The data transmission is split in packages
	// of maximum $OCEANIC_ATOM2_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / OCEANIC_ATOM2_PACKET_SIZE;
		unsigned char answer[OCEANIC_ATOM2_PACKET_SIZE + 1] = {0};
		unsigned char command[4] = {0xB1, 
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0};
		device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer, OCEANIC_ATOM2_PACKET_SIZE);

#ifndef NDEBUG
		message ("ATOM2Read(0x%04x,%d)=\"", address, OCEANIC_ATOM2_PACKET_SIZE);
		for (unsigned int i = 0; i < OCEANIC_ATOM2_PACKET_SIZE; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		nbytes += OCEANIC_ATOM2_PACKET_SIZE;
		address += OCEANIC_ATOM2_PACKET_SIZE;
		data += OCEANIC_ATOM2_PACKET_SIZE;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_read_ringbuffer (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size, unsigned int begin, unsigned int end)
{
	assert (address >= begin && address < end);
	assert (size <= end - begin);

	if (address + size > end) {
		unsigned int a = end - address;
		unsigned int b = size - a;

		device_status_t rc = oceanic_atom2_device_read (abstract, address, data, a);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		rc = oceanic_atom2_device_read (abstract, begin, data + a, b);
		if (rc != DEVICE_STATUS_SUCCESS) 
			return rc;
	} else {
		device_status_t rc = oceanic_atom2_device_read (abstract, address, data, size);
		if (rc != DEVICE_STATUS_SUCCESS) 
			return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}

static device_status_t
oceanic_atom2_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < OCEANIC_ATOM2_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	device_status_t rc = oceanic_atom2_device_read (abstract, 0x00, data, OCEANIC_ATOM2_MEMORY_SIZE);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (result)
		*result = OCEANIC_ATOM2_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Read the pointer data.
	unsigned char pointers[OCEANIC_ATOM2_PACKET_SIZE] = {0};
	device_status_t rc = oceanic_atom2_device_read (abstract, 0x0040, pointers, OCEANIC_ATOM2_PACKET_SIZE);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read pointers.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int logbook_first = PT_LOGBOOK_FIRST (pointers);
	unsigned int logbook_last  = PT_LOGBOOK_LAST (pointers);
	message ("logbook: first=%04x, last=%04x\n", logbook_first, logbook_last);

	// Calculate the total number of logbook entries.
	// In a typical ringbuffer implementation (with only two pointers),
	// there is no distinction between an empty and a full ringbuffer.
	// However, the ATOM2 sets the pointers to a fixed (invalid) value  
	// to indicate an empty buffer. With this knowledge, we can detect
	// the difference between both cases correctly.
	if (logbook_first == RB_LOGBOOK_EMPTY && logbook_last == RB_LOGBOOK_EMPTY)
		return DEVICE_STATUS_SUCCESS;

	unsigned int logbook_count = RB_LOGBOOK_DISTANCE (logbook_first, logbook_last) / 
		(OCEANIC_ATOM2_PACKET_SIZE / 2) + 1;
	message ("logbook: count=%u\n", logbook_count);

	// Align the pointers to the packet size.
	unsigned int logbook_page_offset = logbook_first % OCEANIC_ATOM2_PACKET_SIZE;
	unsigned int logbook_page_first = (logbook_first / OCEANIC_ATOM2_PACKET_SIZE) * OCEANIC_ATOM2_PACKET_SIZE;
	unsigned int logbook_page_last  = (logbook_last  / OCEANIC_ATOM2_PACKET_SIZE) * OCEANIC_ATOM2_PACKET_SIZE;
	unsigned int logbook_page_len = RB_LOGBOOK_DISTANCE (logbook_page_first, logbook_page_last) + OCEANIC_ATOM2_PACKET_SIZE;
	message ("logbook: first=%04x, last=%04x, len=%u, offset=%u\n", 
		logbook_page_first, logbook_page_last, logbook_page_len, logbook_page_offset);

	// Read the logbook data.
	unsigned char logbooks[RB_LOGBOOK_END - RB_LOGBOOK_BEGIN] = {0};
	rc = oceanic_atom2_read_ringbuffer (abstract, logbook_page_first, logbooks, logbook_page_len, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read dive logbooks.");
		return rc;
	}

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	unsigned char *current = logbooks + logbook_page_offset + (logbook_count - 1) * (OCEANIC_ATOM2_PACKET_SIZE / 2);
	for (unsigned int i = 0; i < logbook_count; ++i) {
		message ("logbook: index=%u\n", i);

		// Get the profile pointers.
		unsigned int profile_first = PT_PROFILE_FIRST (current) * OCEANIC_ATOM2_PACKET_SIZE;
		unsigned int profile_last  = PT_PROFILE_LAST (current) * OCEANIC_ATOM2_PACKET_SIZE;
		unsigned int profile_len = RB_PROFILE_DISTANCE (profile_first, profile_last) + OCEANIC_ATOM2_PACKET_SIZE;
		message ("profile: first=%04x, last=%04x, len=%u\n", profile_first, profile_last, profile_len);

		// Read the profile data.
		unsigned char profile[RB_PROFILE_END - RB_PROFILE_BEGIN + 8] = {0};
		rc = oceanic_atom2_read_ringbuffer (abstract, profile_first, profile + 8, profile_len, RB_PROFILE_BEGIN, RB_PROFILE_END);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Cannot read dive profiles.");
			return rc;
		}

		// Copy the logbook data to the profile.
		memcpy (profile, current, 8);

		if (callback && !callback (profile, profile_len + 8, userdata))
			return DEVICE_STATUS_SUCCESS;

		// Advance to the next logbook entry.
		current -= (OCEANIC_ATOM2_PACKET_SIZE / 2);
	}

	return DEVICE_STATUS_SUCCESS;
}
