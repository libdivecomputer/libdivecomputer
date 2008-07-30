#include <stdlib.h> // malloc, free
#include <memory.h> // memcpy

#include "device-private.h"
#include "uwatec_aladin.h"
#include "serial.h"
#include "utils.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define DISTANCE(a,b) ringbuffer_distance (a, b, 0, 0x600)


typedef struct uwatec_aladin_device_t uwatec_aladin_device_t;

struct uwatec_aladin_device_t {
	device_t base;
	struct serial *port;
};

static const device_backend_t uwatec_aladin_device_backend;

static int
device_is_uwatec_aladin (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_aladin_device_backend;
}


device_status_t
uwatec_aladin_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	uwatec_aladin_device_t *device = malloc (sizeof (uwatec_aladin_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &uwatec_aladin_device_backend);

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

	// Set the timeout for receiving data (INFINITE).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the RTS line and set the DTR line.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 0) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_aladin_device_close (device_t *abstract)
{
	uwatec_aladin_device_t *device = (uwatec_aladin_device_t*) abstract;

	if (! device_is_uwatec_aladin (abstract))
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
uwatec_aladin_device_dump (device_t *abstract, unsigned char data[], unsigned int size)
{
	uwatec_aladin_device_t *device = (uwatec_aladin_device_t*) abstract;

	if (! device_is_uwatec_aladin (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char answer[UWATEC_ALADIN_MEMORY_SIZE + 2] = {0};

	// Receive the header of the package.
	for (unsigned int i = 0; i < 4;) {
		int rc = serial_read (device->port, answer + i, 1);
		if (rc != 1) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (rc);
		}
		if (answer[i] == (i < 3 ? 0x55 : 0x00)) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Receive the remaining part of the package.
	int rc = serial_read (device->port, answer + 4, sizeof (answer) - 4);
	if (rc != sizeof (answer) - 4) {
		WARNING ("Unexpected EOF in answer.");
		return EXITCODE (rc);
	}

	// Reverse the bit order.
	array_reverse_bits (answer, sizeof (answer));

	// Verify the checksum of the package.
	unsigned short crc = 
		 answer[UWATEC_ALADIN_MEMORY_SIZE + 0] + 
		(answer[UWATEC_ALADIN_MEMORY_SIZE + 1] << 8);
	unsigned short ccrc = checksum_add_uint16 (answer, UWATEC_ALADIN_MEMORY_SIZE, 0x0000);
	if (ccrc != crc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	if (size >= UWATEC_ALADIN_MEMORY_SIZE) {
		memcpy (data, answer, UWATEC_ALADIN_MEMORY_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return UWATEC_ALADIN_MEMORY_SIZE;
}


static device_status_t
uwatec_aladin_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	unsigned char data[UWATEC_ALADIN_MEMORY_SIZE] = {0};

	int rc = uwatec_aladin_device_dump (abstract, data, sizeof (data));
	if (rc < 0)
		return rc;

	return uwatec_aladin_extract_dives (data, sizeof (data), callback, userdata);
}


#define HEADER 4

device_status_t
uwatec_aladin_extract_dives (const unsigned char* data, unsigned int size, dive_callback_t callback, void *userdata)
{
	if (size < UWATEC_ALADIN_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	// The logbook ring buffer can store up to 37 dives. But
	// if the total number of dives is less, not all logbook
	// entries contain valid data.
	unsigned int ndives = (data[HEADER + 0x7f2] << 8) + data[HEADER + 0x7f3];
	if (ndives > 37)
		ndives = 37;

	// Get the index to the newest logbook entry. This value is
	// normally in the range from 1 to 37 and is converted to
	// a zero based index, taking care not to underflow.
	unsigned int eol = (data[HEADER + 0x7f4] + 37 - 1) % 37;

	// Get the end of the profile ring buffer. This value points
	// to the last byte of the last profile and is incremented
	// one byte to point immediately after the last profile.
	unsigned int eop = (data[HEADER + 0x7f6] + 
		(((data[HEADER + 0x7f7] & 0x0F) >> 1) << 8) + 1) % 0x600;

	// Start scanning the profile ringbuffer.
	int profiles = 1;

	// Both ring buffers are traversed backwards to retrieve the most recent
	// dives first. This allows you to download only the new dives and avoids 
	// having to rely on the number of profiles in the ring buffer (which
	// is buggy according to the documentation). During the traversal, the 
	// previous pointer does always point to the end of the dive data and 
	// we move the current pointer backwards until a start marker is found.
	unsigned int previous = eop;
	unsigned int current = eop;
	for (unsigned int i = 0; i < ndives; ++i) {
		// Memory buffer to store one dive.
		unsigned char buffer[18 + 0x600] = {0};

		// Get the offset to the current logbook entry.
		unsigned int offset = ((eol + 37 - i) % 37) * 12 + 0x600;

		// Copy the serial number, type and logbook data
		// to the buffer and set the profile length to zero.
		memcpy (buffer + 0, data + HEADER + 0x07ed, 3);
		memcpy (buffer + 3, data + HEADER + 0x07bc, 1);
		memcpy (buffer + 4, data + HEADER + offset, 12);
		memset (buffer + 16, 0, 2);

		// Convert the timestamp from the Aladin (big endian)
		// to the Memomouse format (little endian).
		array_reverse_bytes (buffer + 11, 4);

		unsigned int len = 0;
		if (profiles) {
			// Search the profile ringbuffer for a start marker.
			do {
				if (current == 0)
					current = 0x600;
				current--;

				if (data[HEADER + current] == 0xFF) {
					len = DISTANCE (current, previous);
					previous = current;
					break;
				}
			} while (current != eop);

			if (len >= 1) {		
				// Skip the start marker.
				len--;
				unsigned int begin = (current + 1) % 0x600;
				// Set the profile length.
				buffer[16] = (len     ) & 0xFF;
				buffer[17] = (len >> 8) & 0xFF;
				// Copy the profile data.
				if (begin + len > 0x600) {
					unsigned int a = 0x600 - begin;
					unsigned int b = (begin + len) - 0x600;
					memcpy (buffer + 18 + 0, data + HEADER + begin, a);
					memcpy (buffer + 18 + a, data + HEADER,         b);
				} else {
					memcpy (buffer + 18, data + HEADER + begin, len);
				}
			}

			// Since the size of the profile ringbuffer is limited,
			// not all logbook entries will have profile data. Thus,
			// once the end of the profile ringbuffer is reached,
			// there is no need to keep scanning the ringbuffer.
			if (current == eop)
				profiles = 0;
		}

		if (callback && !callback (buffer, len + 18, userdata))
			return DEVICE_STATUS_SUCCESS;
	}

	return DEVICE_STATUS_SUCCESS;
}


static const device_backend_t uwatec_aladin_device_backend = {
	DEVICE_TYPE_UWATEC_ALADIN,
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	uwatec_aladin_device_dump, /* dump */
	uwatec_aladin_device_foreach, /* foreach */
	uwatec_aladin_device_close /* close */
};
