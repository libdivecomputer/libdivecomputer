/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "oceanic_atom2.h"
#include "serial.h"
#include "utils.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define MAXRETRIES 2

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define FP_OFFSET 0
#define FP_SIZE   8

#define ACK 0x5A
#define NAK 0xA5

#define CF_POINTERS					0x0040

#define RB_LOGBOOK_EMPTY			0x0230
#define RB_LOGBOOK_BEGIN			0x0240
#define RB_LOGBOOK_END				0x0A40
#define RB_LOGBOOK_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END)
#define RB_LOGBOOK_INCR(a,b)		ringbuffer_increment (a, b, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END)

#define RB_PROFILE_EMPTY			0x0A40
#define RB_PROFILE_BEGIN			0x0A50
#define RB_PROFILE_END				0xFFF0
#define RB_PROFILE_DISTANCE(a,b)	ringbuffer_distance (a, b, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define PT_PROFILE_FIRST(x)			(((array_uint16_le ((x) + 5)     ) & 0x0FFF) * OCEANIC_ATOM2_PACKET_SIZE)
#define PT_PROFILE_LAST(x)			(((array_uint16_le ((x) + 6) >> 4) & 0x0FFF) * OCEANIC_ATOM2_PACKET_SIZE)

typedef struct oceanic_atom2_device_t {
	device_t base;
	struct serial *port;
	unsigned char fingerprint[FP_SIZE];
} oceanic_atom2_device_t;

static device_status_t oceanic_atom2_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_version (device_t *abstract, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t oceanic_atom2_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t oceanic_atom2_device_close (device_t *abstract);

static const device_backend_t oceanic_atom2_device_backend = {
	DEVICE_TYPE_OCEANIC_ATOM2,
	oceanic_atom2_device_set_fingerprint, /* set_fingerprint */
	NULL, /* handshake */
	oceanic_atom2_device_version, /* version */
	oceanic_atom2_device_read, /* read */
	oceanic_atom2_device_write, /* write */
	oceanic_atom2_device_dump, /* dump */
	oceanic_atom2_device_foreach, /* foreach */
	oceanic_atom2_device_close /* close */
};


static unsigned int
ifloor (unsigned int x, unsigned int n)
{
	// Round down to next lower multiple.
	return (x / n) * n;
}


static unsigned int
iceil (unsigned int x, unsigned int n)
{
	// Round up to next higher multiple.
	return ((x + n - 1) / n) * n;
}


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


static device_status_t
oceanic_atom2_init (oceanic_atom2_device_t *device)
{
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


static device_status_t
oceanic_atom2_quit (oceanic_atom2_device_t *device)
{
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
	memset (device->fingerprint, 0, FP_SIZE);

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

	// Send the init command.
	oceanic_atom2_init (device);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != FP_SIZE)
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, FP_SIZE);
	else
		memset (device->fingerprint, 0, FP_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_close (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the quit command.
	oceanic_atom2_quit (device);

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
oceanic_atom2_device_keepalive (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[4] = {0x91, 0x05, 0xA5, 0x00};
	device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

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
oceanic_atom2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
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
		// Prepare to write the package.
		unsigned int number = address / OCEANIC_ATOM2_PACKET_SIZE;
		unsigned char prepare[4] = {0xB2,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0x00};
		device_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), NULL, 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("ATOM2PrepareWrite(0x%04x,%d)\n", address, OCEANIC_ATOM2_PACKET_SIZE);
#endif

		// Write the package.
		unsigned char command[OCEANIC_ATOM2_PACKET_SIZE + 2] = {0};
		memcpy (command, data, OCEANIC_ATOM2_PACKET_SIZE);
		command[OCEANIC_ATOM2_PACKET_SIZE] = checksum_add_uint8 (command, OCEANIC_ATOM2_PACKET_SIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("ATOM2Write(0x%04x,%d)=\"", address, OCEANIC_ATOM2_PACKET_SIZE);
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

	if (size < OCEANIC_ATOM2_MEMORY_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

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
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Read the pointer data.
	unsigned char pointers[OCEANIC_ATOM2_PACKET_SIZE] = {0};
	device_status_t rc = oceanic_atom2_device_read (abstract, CF_POINTERS, pointers, OCEANIC_ATOM2_PACKET_SIZE);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read pointers.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int rb_logbook_first = array_uint16_le (pointers + 4);
	unsigned int rb_logbook_last  = array_uint16_le (pointers + 6);

	// Convert the first/last pointers to begin/end/count pointers.
	unsigned int rb_logbook_entry_begin, rb_logbook_entry_end,
		rb_logbook_entry_size;
	if (rb_logbook_first == RB_LOGBOOK_EMPTY &&
		rb_logbook_last == RB_LOGBOOK_EMPTY)
	{
		// Empty ringbuffer.
		rb_logbook_entry_begin = RB_LOGBOOK_BEGIN;
		rb_logbook_entry_end   = RB_LOGBOOK_BEGIN;
		rb_logbook_entry_size  = 0;
	} else {
		// Non-empty ringbuffer.
		rb_logbook_entry_begin = rb_logbook_first;
		rb_logbook_entry_end   = RB_LOGBOOK_INCR (rb_logbook_last, OCEANIC_ATOM2_PACKET_SIZE / 2);
		rb_logbook_entry_size  = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_last) + OCEANIC_ATOM2_PACKET_SIZE / 2;
	}

	// Check whether the ringbuffer is full.
	int full = (rb_logbook_entry_size == (RB_LOGBOOK_END - RB_LOGBOOK_BEGIN));

	// Align the pointers to page boundaries.
	unsigned int rb_logbook_page_begin, rb_logbook_page_end,
		rb_logbook_page_size;
	if (full) {
		// Full ringbuffer.
		rb_logbook_page_begin = iceil (rb_logbook_entry_end, OCEANIC_ATOM2_PACKET_SIZE);
		rb_logbook_page_end   = rb_logbook_page_begin;
		rb_logbook_page_size  = rb_logbook_entry_size;
	} else {
		// Non-full ringbuffer.
		rb_logbook_page_begin = ifloor (rb_logbook_entry_begin, OCEANIC_ATOM2_PACKET_SIZE);
		rb_logbook_page_end   = iceil (rb_logbook_entry_end, OCEANIC_ATOM2_PACKET_SIZE);
		rb_logbook_page_size  = rb_logbook_entry_size +
			(rb_logbook_entry_begin - rb_logbook_page_begin) +
			(rb_logbook_page_end - rb_logbook_entry_end);
	}

	// Check whether the last entry is not aligned to a page boundary.
	int unaligned = (rb_logbook_entry_end != rb_logbook_page_end);

	// Memory buffer for the logbook entries.
	unsigned char logbooks[RB_LOGBOOK_END - RB_LOGBOOK_BEGIN] = {0};

	// Since entries are not necessary aligned on page boundaries,
	// the memory buffer may contain padding entries on both sides.
	// The memory area which contains the valid entries is marked
	// with a number of additional variables.
	unsigned int begin = 0;
	unsigned int end = rb_logbook_page_size;
	if (!full) {
		begin += rb_logbook_entry_begin - rb_logbook_page_begin;
		end -= rb_logbook_page_end - rb_logbook_entry_end;
	}

	// The logbook ringbuffer is read backwards to retrieve the most recent
	// entries first. If an already downloaded entry is identified (by means
	// of its fingerprint), the transfer is aborted immediately to reduce
	// the transfer time. When necessary, padding entries are downloaded
	// (but not processed) to align all read requests on page boundaries.
	unsigned int entry = end;
	unsigned int page = rb_logbook_page_size;
	unsigned int address = rb_logbook_page_end;
	unsigned int npages = rb_logbook_page_size / OCEANIC_ATOM2_PACKET_SIZE;
	for (unsigned int i = 0; i < npages; ++i) {
		// Move to the start of the current page.
		if (address == RB_LOGBOOK_BEGIN)
			address = RB_LOGBOOK_END;
		address -= OCEANIC_ATOM2_PACKET_SIZE;
		page -= OCEANIC_ATOM2_PACKET_SIZE;

		// Read the logbook page.
		rc = oceanic_atom2_device_read (abstract, address, logbooks + page, OCEANIC_ATOM2_PACKET_SIZE);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// A full ringbuffer needs some special treatment to avoid
		// having to download the first/last page twice. When a full
		// ringbuffer is not aligned to page boundaries, this page
		// will contain both the most recent and oldest entry.
		if (full && unaligned) {
			if (i == 0) {
				// After downloading the first page, move both the oldest
				// and most recent entries to their correct location.
				unsigned int oldest = rb_logbook_page_end - rb_logbook_entry_end;
				unsigned int newest  = OCEANIC_ATOM2_PACKET_SIZE - oldest;
				// Move the oldest entries down to the start of the buffer.
				memcpy (logbooks, logbooks + page + newest, oldest);
				// Move the newest entries up to the end of the buffer.
				memmove (logbooks + page + oldest, logbooks + page, newest);
				// Adjust the current page offset to the new position.
				page += oldest;
			} else if (i == npages - 1) {
				// After downloading the last page, pretend we have also
				// downloaded those oldest entries from the first page.
				page = 0;
			}
		}

		// Process the logbook entries.
		int abort = 0;
		while (entry != page && entry != begin) {
			// Move to the start of the current entry.
			entry -= OCEANIC_ATOM2_PACKET_SIZE / 2;

			// Compare the fingerprint to identify previously downloaded entries.
			if (memcmp (logbooks + entry + FP_OFFSET, device->fingerprint, FP_SIZE) == 0) {
				begin = entry + OCEANIC_ATOM2_PACKET_SIZE / 2;
				abort = 1;
				break;
			}
		}

		// Stop reading pages too.
		if (abort)
			break;
	}

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	entry = end;
	while (entry != begin) {
		// Move to the start of the current entry.
		entry -= OCEANIC_ATOM2_PACKET_SIZE / 2;

		// Get the profile pointers.
		unsigned int rb_entry_first = PT_PROFILE_FIRST (logbooks + entry);
		unsigned int rb_entry_last  = PT_PROFILE_LAST (logbooks + entry);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last) + OCEANIC_ATOM2_PACKET_SIZE;

		// Read the profile data.
		unsigned char profile[(RB_PROFILE_END - RB_PROFILE_BEGIN) + OCEANIC_ATOM2_PACKET_SIZE / 2] = {0};
		rc = oceanic_atom2_read_ringbuffer (abstract, rb_entry_first, profile + OCEANIC_ATOM2_PACKET_SIZE / 2, rb_entry_size, RB_PROFILE_BEGIN, RB_PROFILE_END);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Cannot read dive profiles.");
			return rc;
		}

		// Copy the logbook data to the profile.
		memcpy (profile, logbooks + entry, OCEANIC_ATOM2_PACKET_SIZE / 2);

		if (callback && !callback (profile, rb_entry_size + OCEANIC_ATOM2_PACKET_SIZE / 2, userdata))
			return DEVICE_STATUS_SUCCESS;
	}

	return DEVICE_STATUS_SUCCESS;
}
