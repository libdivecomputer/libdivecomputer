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

#include "mares_common.h"
#include "utils.h"
#include "array.h"

#define FP_OFFSET 8
#define FP_SIZE   5

void
mares_common_device_init (mares_common_device_t *device, const device_backend_t *backend)
{
	assert (device != NULL);

	// Initialize the base class.
	device_init (&device->base, backend);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->layout = NULL;
}


device_status_t
mares_common_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_common_device_t *device = (mares_common_device_t *) abstract;

	assert (device != NULL);

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
mares_common_extract_dives (mares_common_device_t *device, const mares_common_layout_t *layout, const unsigned char data[], dive_callback_t callback, void *userdata)
{
	assert (layout != NULL);

	// Get the freedive mode for this model.
	unsigned int freedive = 2;
	if (data[1] == 1 || data[1] == 7)
		freedive = 3;

	// Get the end of the profile ring buffer.
	unsigned int eop = array_uint16_le (data + 0x6B);
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		WARNING ("Ringbuffer pointer out of range.");
		return DEVICE_STATUS_ERROR;
	}

	// Make the ringbuffer linear, to avoid having to deal
	// with the wrap point. The buffer has extra space to
	// store the profile data for the freedives.
	unsigned char *buffer = (unsigned char *) malloc (
		layout->rb_profile_end - layout->rb_profile_begin +
		layout->rb_freedives_end - layout->rb_freedives_begin);
	if (buffer == NULL) {
		WARNING ("Out of memory.");
		return DEVICE_STATUS_MEMORY;
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
		unsigned int sample_size = (extra ? 5 : 2);
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
			WARNING ("Calculated and stored size are not equal.");
			free (buffer);
			return DEVICE_STATUS_ERROR;
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
			assert (count == nsamples);

			// Append the profile data to the main logbook entry. The
			// buffer is guaranteed to have enough space, and the dives
			// that will be overwritten have already been processed.
			memcpy (buffer + offset + nbytes, data + layout->rb_freedives_begin, idx - layout->rb_freedives_begin);
			nbytes += idx - layout->rb_freedives_begin;
		}

		unsigned int fp_offset = offset + length - extra - FP_OFFSET;
		if (device && memcmp (buffer + fp_offset, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (callback && !callback (buffer + offset, nbytes, buffer + fp_offset, sizeof (device->fingerprint), userdata)) {
			free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}
	}

	free (buffer);

	return DEVICE_STATUS_SUCCESS;
}
