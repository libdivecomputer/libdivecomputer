/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <stdlib.h> // malloc
#include <string.h> // memcpy, memcmp
#include <assert.h> // assert

#include "context-private.h"
#include "mares_common.h"
#include "checksum.h"
#include "array.h"

#define MAXRETRIES 4

#define FP_OFFSET 8
#define FP_SIZE   5

#define NEMO        0
#define NEMOWIDE    1
#define NEMOAIR     4
#define PUCK        7
#define NEMOEXCEL   17
#define NEMOAPNEIST 18
#define PUCKAIR     19

#define AIR      0
#define NITROX   1
#define FREEDIVE 2
#define GAUGE    3

void
mares_common_device_init (mares_common_device_t *device)
{
	assert (device != NULL);

	// Set the default values.
	device->port = NULL;
	device->echo = 0;
	device->delay = 0;
}


static void
mares_common_make_ascii (const unsigned char raw[], unsigned int rsize, unsigned char ascii[], unsigned int asize)
{
	assert (asize == 2 * (rsize + 2));

	// Header
	ascii[0] = '<';

	// Data
	array_convert_bin2hex (raw, rsize, ascii + 1, 2 * rsize);

	// Checksum
	unsigned char checksum = checksum_add_uint8 (ascii + 1, 2 * rsize, 0x00);
	array_convert_bin2hex (&checksum, 1, ascii + 1 + 2 * rsize, 2);

	// Trailer
	ascii[asize - 1] = '>';
}


static dc_status_t
mares_common_packet (mares_common_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (device->delay) {
		dc_serial_sleep (device->port, device->delay);
	}

	// Send the command to the device.
	status = dc_serial_write (device->port, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	if (device->echo) {
		// Receive the echo of the command.
		unsigned char echo[PACKETSIZE] = {0};
		status = dc_serial_read (device->port, echo, csize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the echo.");
			return status;
		}

		// Verify the echo.
		if (memcmp (echo, command, csize) != 0) {
			WARNING (abstract->context, "Unexpected echo.");
		}
	}

	// Receive the answer of the device.
	status = dc_serial_read (device->port, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header and trailer of the packet.
	if (answer[0] != '<' || answer[asize - 1] != '>') {
		ERROR (abstract->context, "Unexpected answer header/trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned char crc = 0;
	unsigned char ccrc = checksum_add_uint8 (answer + 1, asize - 4, 0x00);
	array_convert_hex2bin (answer + asize - 3, 2, &crc, 1);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_common_transfer (mares_common_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = mares_common_packet (device, command, csize, answer, asize)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Discard any garbage bytes.
		dc_serial_sleep (device->port, 100);
		dc_serial_purge (device->port, DC_DIRECTION_INPUT);
	}

	return rc;
}


dc_status_t
mares_common_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	mares_common_device_t *device = (mares_common_device_t*) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > PACKETSIZE)
			len = PACKETSIZE;

		// Build the raw command.
		unsigned char raw[] = {0x51,
			(address     ) & 0xFF, // Low
			(address >> 8) & 0xFF, // High
			len}; // Count

		// Build the ascii command.
		unsigned char command[2 * (sizeof (raw) + 2)] = {0};
		mares_common_make_ascii (raw, sizeof (raw), command, sizeof (command));

		// Send the command and receive the answer.
		unsigned char answer[2 * (PACKETSIZE + 2)] = {0};
		dc_status_t rc = mares_common_transfer (device, command, sizeof (command), answer, 2 * (len + 2));
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Extract the raw data from the packet.
		array_convert_hex2bin (answer + 1, 2 * len, data, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
mares_common_extract_dives (dc_context_t *context, const mares_common_layout_t *layout, const unsigned char fingerprint[], const unsigned char data[], dc_dive_callback_t callback, void *userdata)
{
	assert (layout != NULL);

	// Get the freedive mode for this model.
	unsigned int model = data[1];
	unsigned int freedive = FREEDIVE;
	if (model == NEMOWIDE || model == NEMOAIR || model == PUCK || model == PUCKAIR)
		freedive = GAUGE;

	// Get the end of the profile ring buffer.
	unsigned int eop = array_uint16_le (data + 0x6B);
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		ERROR (context, "Ringbuffer pointer out of range (0x%04x).", eop);
		return DC_STATUS_DATAFORMAT;
	}

	// Make the ringbuffer linear, to avoid having to deal
	// with the wrap point. The buffer has extra space to
	// store the profile data for the freedives.
	unsigned char *buffer = (unsigned char *) malloc (
		layout->rb_profile_end - layout->rb_profile_begin +
		layout->rb_freedives_end - layout->rb_freedives_begin);
	if (buffer == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	memcpy (buffer + 0, data + eop, layout->rb_profile_end - eop);
	memcpy (buffer + layout->rb_profile_end - eop, data + layout->rb_profile_begin, eop - layout->rb_profile_begin);

	// For a freedive session, the Mares Nemo stores all the freedives of
	// that session in a single logbook entry, and each sample is actually
	// a summary for each individual freedive in the session. The profile
	// data is stored in a separate memory area. Since only the most recent
	// recent freediving session can have profile data, we keep track of the
	// number of freedives.
	unsigned int nfreedives = 0;

	unsigned int offset = layout->rb_profile_end - layout->rb_profile_begin;
	while (offset >= 3) {
		// Check for the presence of extra header bytes, which can be detected
		// by means of a three byte marker sequence.
		unsigned int extra = 0;
		const unsigned char marker[3] = {0xAA, 0xBB, 0xCC};
		if (memcmp (buffer + offset - 3, marker, sizeof (marker)) == 0) {
			if (model == PUCKAIR)
				extra = 7;
			else
				extra = 12;
		}

		// Check for overflows due to incomplete dives.
		if (offset < extra + 3)
			break;

		// Check the dive mode of the logbook entry. Valid modes are
		// 0 (air), 1 (EANx), 2 (freedive) or 3 (bottom timer).
		// If the ringbuffer has never reached the wrap point before,
		// there will be "empty" memory (filled with 0xFF) and
		// processing should stop at this point.
		unsigned int mode = buffer[offset - extra - 1];
		if (mode == 0xFF)
			break;

		// The header and sample size are dependant on the dive mode. Only
		// in freedive mode, the sizes are different from the other modes.
		unsigned int header_size = 53;
		unsigned int sample_size = 2;
		if (extra) {
			if (model == PUCKAIR)
				sample_size = 3;
			else
				sample_size = 5;
		}
		if (mode == freedive) {
			header_size = 28;
			sample_size = 6;
			nfreedives++;
		}

		// Get the number of samples in the profile data.
		unsigned int nsamples = array_uint16_le (buffer + offset - extra - 3);

		// Calculate the total number of bytes for this dive.
		// If the buffer does not contain that much bytes, we reached the
		// end of the ringbuffer. The current dive is incomplete (partially
		// overwritten with newer data), and processing should stop.
		unsigned int nbytes = 2 + nsamples * sample_size + header_size + extra;
		if (offset < nbytes)
			break;

		// Move to the start of the dive.
		offset -= nbytes;

		// Verify that the length that is stored in the profile data
		// equals the calculated length. If both values are different,
		// something is wrong and an error is returned.
		unsigned int length = array_uint16_le (buffer + offset);
		if (length != nbytes) {
			ERROR (context, "Calculated and stored size are not equal (%u %u).", length, nbytes);
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		// Process the profile data for the most recent freedive entry.
		// Since we are processing the entries backwards (newest to oldest),
		// this entry will always be the first one.
		if (mode == freedive && nfreedives == 1) {
			// Count the number of freedives in the profile data.
			unsigned int count = 0;
			unsigned int idx = layout->rb_freedives_begin;
			while (idx + 2 <= layout->rb_freedives_end &&
				count != nsamples)
			{
				// Each freedive in the session ends with a zero sample.
				unsigned int sample = array_uint16_le (data + idx);
				if (sample == 0)
					count++;

				// Move to the next sample.
				idx += 2;
			}

			// Verify that the number of freedive entries in the session
			// equals the number of freedives in the profile data. If
			// both values are different, the profile data is incomplete.
			if (count != nsamples) {
				ERROR (context, "Unexpected number of freedive sessions (%u %u).", count, nsamples);
				free (buffer);
				return DC_STATUS_DATAFORMAT;
			}

			// Append the profile data to the main logbook entry. The
			// buffer is guaranteed to have enough space, and the dives
			// that will be overwritten have already been processed.
			memcpy (buffer + offset + nbytes, data + layout->rb_freedives_begin, idx - layout->rb_freedives_begin);
			nbytes += idx - layout->rb_freedives_begin;
		}

		unsigned int fp_offset = offset + length - extra - FP_OFFSET;
		if (fingerprint && memcmp (buffer + fp_offset, fingerprint, FP_SIZE) == 0) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (buffer + offset, nbytes, buffer + fp_offset, FP_SIZE, userdata)) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}
