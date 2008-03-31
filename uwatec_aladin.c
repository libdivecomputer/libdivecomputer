#include <stdlib.h> // malloc, free
#include <memory.h> // memcpy

#include "uwatec.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


struct aladin {
	struct serial *port;
};


#define DISTANCE(a,b) distance (a, b, 0x600)

static unsigned int
distance (unsigned int a, unsigned int b, unsigned int size)
{
	if (a <= b) {
		return (b - a) % size;
	} else {
		return size - (a - b) % size;
	}
}


int
uwatec_aladin_open (aladin **out, const char* name)
{
	if (out == NULL)
		return UWATEC_ERROR;

	// Allocate memory.
	struct aladin *device = malloc (sizeof (struct aladin));
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

	// Set the serial communication protocol (19200 8N1).
	rc = serial_configure (device->port, 19200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

	// Set the timeout for receiving data (INFINITE).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return UWATEC_ERROR_IO;
	}

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
uwatec_aladin_close (aladin *device)
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
uwatec_aladin_reverse_bits (unsigned char data[], unsigned int size)
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


static void
uwatec_aladin_reverse_bytes (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size / 2; ++i) {
		unsigned char hlp = data[i];
		data[i] = data[size-1-i];
		data[size-1-i] = hlp;
	}
}


static unsigned short
uwatec_aladin_checksum (unsigned char data[], unsigned int size)
{
	unsigned short crc = 0x00;
	for (unsigned int i = 0; i < size; ++i)
		crc += data[i];

	return crc;
}


int
uwatec_aladin_read (aladin *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return UWATEC_ERROR;

	if (size < UWATEC_ALADIN_MEMORY_SIZE)
		return UWATEC_ERROR_MEMORY;

	// Receive the header of the package.
	for (unsigned int i = 0; i < 4;) {
		int rc = serial_read (device->port, data + i, 1);
		if (rc != 1) {
			WARNING ("Cannot read from device.");
			return UWATEC_ERROR;
		}
		if (data[i] == (i < 3 ? 0x55 : 0x00)) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Receive the contents of the package.
	int rc = serial_read (device->port, data + 4, UWATEC_ALADIN_MEMORY_SIZE - 4);
	if (rc != UWATEC_ALADIN_MEMORY_SIZE - 4) {
		WARNING ("Unexpected EOF in answer.");
		return UWATEC_ERROR;
	}

	// Reverse the bit order.
	uwatec_aladin_reverse_bits (data, UWATEC_ALADIN_MEMORY_SIZE);

	// Calculate the checksum.
	unsigned short ccrc = uwatec_aladin_checksum (data, UWATEC_ALADIN_MEMORY_SIZE);

	// Receive (and verify) the checksum of the package.
	unsigned char checksum[2] = {0};
	rc = serial_read (device->port, checksum, sizeof (checksum));
	uwatec_aladin_reverse_bits (checksum, sizeof (checksum));
	unsigned short crc = (checksum[1] << 8) + checksum[0];
	if (rc != sizeof (checksum) || ccrc != crc) {
		WARNING ("Unexpected answer CRC.");
		return UWATEC_ERROR;
	}

	return UWATEC_SUCCESS;
}


#define HEADER 4

int
uwatec_aladin_extract_dives (const unsigned char* data, unsigned int size, dive_callback_t callback, void *userdata)
{
	if (size < UWATEC_ALADIN_MEMORY_SIZE)
		return UWATEC_ERROR;

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
		uwatec_aladin_reverse_bytes (buffer + 11, 4);

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

		if (callback)
			callback (buffer, len + 18, userdata);
	}

	return UWATEC_SUCCESS;
}
