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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "mares_nemo.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)


#define RB_PROFILE_BEGIN			0x0070
#define RB_PROFILE_END				0x3400
#define RB_FREEDIVES_BEGIN			0x3400
#define RB_FREEDIVES_END			0x4000

typedef struct mares_nemo_device_t mares_nemo_device_t;

struct mares_nemo_device_t {
	device_t base;
	struct serial *port;
};

static device_status_t mares_nemo_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t mares_nemo_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t mares_nemo_device_close (device_t *abstract);

static const device_backend_t mares_nemo_device_backend = {
	DEVICE_TYPE_MARES_NEMO,
	NULL, /* set_fingerprint */
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	mares_nemo_device_dump, /* dump */
	mares_nemo_device_foreach, /* foreach */
	mares_nemo_device_close /* close */
};

static int
device_is_mares_nemo (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_nemo_device_backend;
}


device_status_t
mares_nemo_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_nemo_device_t *device = (mares_nemo_device_t *) malloc (sizeof (mares_nemo_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &mares_nemo_device_backend);

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

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_nemo_device_close (device_t *abstract)
{
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;

	if (! device_is_mares_nemo (abstract))
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
mares_nemo_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	if (! device_is_mares_nemo (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < MARES_NEMO_MEMORY_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = MARES_NEMO_MEMORY_SIZE + 20;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Receive the header of the package.
	unsigned char header = 0x00;
	for (unsigned int i = 0; i < 20;) {
		int n = serial_read (device->port, &header, 1);
		if (n != 1) {
			WARNING ("Failed to receive the header.");
			return EXITCODE (n);
		}
		if (header == 0xEE) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Update and emit a progress event.
	progress.current += 20;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	unsigned int nbytes = 0;
	while (nbytes < MARES_NEMO_MEMORY_SIZE) {
		// Read the packet.
		unsigned char packet[(MARES_NEMO_PACKET_SIZE + 1) * 2] = {0};
		int n = serial_read (device->port, packet, sizeof (packet));
		if (n != sizeof (packet)) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Verify the checksums of the packet.
		unsigned char crc1 = packet[MARES_NEMO_PACKET_SIZE];
		unsigned char crc2 = packet[MARES_NEMO_PACKET_SIZE * 2 + 1];
		unsigned char ccrc1 = checksum_add_uint8 (packet, MARES_NEMO_PACKET_SIZE, 0x00);
		unsigned char ccrc2 = checksum_add_uint8 (packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE, 0x00);
		if (crc1 == ccrc1 && crc2 == ccrc2) {
			// Both packets have a correct checksum.
			if (memcmp (packet, packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE) != 0) {
				WARNING ("Both packets are not equal.");
				return DEVICE_STATUS_PROTOCOL;
			}
			memcpy (data + nbytes, packet, MARES_NEMO_PACKET_SIZE);
		} else if (crc1 == ccrc1) {
			// Only the first packet has a correct checksum.
			WARNING ("Only the first packet has a correct checksum.");
			memcpy (data + nbytes, packet, MARES_NEMO_PACKET_SIZE);
		} else if (crc2 == ccrc2) {
			// Only the second packet has a correct checksum.
			WARNING ("Only the second packet has a correct checksum.");
			memcpy (data + nbytes, packet + MARES_NEMO_PACKET_SIZE + 1, MARES_NEMO_PACKET_SIZE);
		} else {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		progress.current += MARES_NEMO_PACKET_SIZE;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += MARES_NEMO_PACKET_SIZE;
	}

	if (result)
		*result = MARES_NEMO_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_nemo_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_mares_nemo (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char data[MARES_NEMO_MEMORY_SIZE] = {0};

	device_status_t rc = mares_nemo_device_dump (abstract, data, sizeof (data), NULL);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return mares_nemo_extract_dives (data, sizeof (data), callback, userdata);
}


device_status_t
mares_nemo_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= MARES_NEMO_MEMORY_SIZE);

	// Get the end of the profile ring buffer.
	unsigned int eop = data[0x6B] + (data[0x6C] << 8);

	// Make the ringbuffer linear, to avoid having to deal
	// with the wrap point. The buffer has extra space to
	// store the profile data for the freedives.
	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN + RB_FREEDIVES_END - RB_FREEDIVES_BEGIN] = {0};
	memcpy (buffer + 0, data + eop, RB_PROFILE_END - eop);
	memcpy (buffer + RB_PROFILE_END - eop, data + RB_PROFILE_BEGIN, eop - RB_PROFILE_BEGIN);

	// For a freedive session, the Mares Nemo stores all the freedives of
	// that session in a single logbook entry, and each sample is actually
	// a summary for each individual freedive in the session. The profile
	// data is stored in a separate memory area. Since only the most recent
	// recent freediving session can have profile data, we keep track of the
	// number of freedives.
	unsigned int nfreedives = 0;

	unsigned int offset = RB_PROFILE_END - RB_PROFILE_BEGIN;
	while (offset >= 3) {
		// Check the dive mode of the logbook entry. Valid modes are
		// 0 (air), 1 (EANx), 2 (freedive) or 3 (bottom timer).
		// If the ringbuffer has never reached the wrap point before,
		// there will be "empty" memory (filled with 0xFF) and
		// processing should stop at this point.
		unsigned int mode = buffer[offset - 1];
		if (mode == 0xFF)
			break;

		// The header and sample size are dependant on the dive mode. Only
		// in freedive mode, the sizes are different from the other modes.
		unsigned int header_size = 53;
		unsigned int sample_size = 2;
		if (mode == 2) {
			header_size = 28;
			sample_size = 6;
			nfreedives++;
		}

		// Get the number of samples in the profile data.
		unsigned int nsamples = buffer[offset - 3] + (buffer[offset - 2] << 8);

		// Calculate the total number of bytes for this dive.
		// If the buffer does not contain that much bytes, we reached the
		// end of the ringbuffer. The current dive is incomplete (partially
		// overwritten with newer data), and processing should stop.
		unsigned int nbytes = 2 + nsamples * sample_size + header_size;
		if (offset < nbytes)
			break;

		// Move to the start of the dive.
		offset -= nbytes;

		// Verify that the length that is stored in the profile data
		// equals the calculated length. If both values are different,
		// something is wrong and an error is returned.
		unsigned int length = buffer[offset] + (buffer[offset + 1] << 8);
		if (length != nbytes) {
			WARNING ("Calculated and stored size are not equal.");
			return DEVICE_STATUS_ERROR;
		}

		// Process the profile data for the most recent freedive entry.
		// Since we are processing the entries backwards (newest to oldest),
		// this entry will always be the first one.
		if (mode == 2 && nfreedives == 1) {
			// Count the number of freedives in the profile data.
			unsigned int count = 0;
			unsigned int idx = RB_FREEDIVES_BEGIN;
			while (idx + 2 <= RB_FREEDIVES_END &&
				count != nsamples)
			{
				// Each freedive in the session ends with a zero sample.
				unsigned int sample = data[idx] + (data[idx + 1] << 8);
				if (sample == 0)
					count++;

				// Move to the next sample.
				idx += 2;
			}

			// Verify that the number of freedive entries in the session
			// equals the number of freedives in the profile data. If
			// both values are different, the profile data is incomplete.
			assert (count == nsamples);

			// Append the profile data to the main logbook entry. The
			// buffer is guaranteed to have enough space, and the dives
			// that will be overwritten have already been processed.
			memcpy (buffer + offset + nbytes, data + RB_FREEDIVES_BEGIN, idx - RB_FREEDIVES_BEGIN);
			nbytes += idx - RB_FREEDIVES_BEGIN;
		}

		if (callback && !callback (buffer + offset, nbytes, userdata))
			return DEVICE_STATUS_SUCCESS;
	}

	return DEVICE_STATUS_SUCCESS;
}
