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

#include <stdlib.h> // malloc
#include <string.h> // memcpy, memcmp
#include <assert.h> // assert

#include "suunto_common.h"
#include "ringbuffer.h"
#include "array.h"

#define RB_PROFILE_DISTANCE(a,b,l)	ringbuffer_distance (a, b, DC_RINGBUFFER_EMPTY, l->rb_profile_begin, l->rb_profile_end)
#define RB_PROFILE_PEEK(a,l)		ringbuffer_decrement (a, l->peek, l->rb_profile_begin, l->rb_profile_end)

void
suunto_common_device_init (suunto_common_device_t *device)
{
	assert (device != NULL);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
}


dc_status_t
suunto_common_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_common_device_t *device = (suunto_common_device_t *) abstract;

	assert (device != NULL);

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common_extract_dives (suunto_common_device_t *device, const suunto_common_layout_t *layout, const unsigned char data[], dc_dive_callback_t callback, void *userdata)
{
	assert (layout != NULL);

	unsigned int eop;
	if (layout->eop) {
		// Get the end-of-profile pointer directly from the header.
		eop = array_uint16_be (data + layout->eop);
	} else {
		// Get the end-of-profile pointer by searching for the
		// end-of-profile marker in the profile ringbuffer.
		eop = layout->rb_profile_begin;
		while (eop < layout->rb_profile_end) {
			if (data[eop] == 0x82)
				break;
			eop++;
		}
	}

	// Validate the end-of-profile pointer.
	if (eop < layout->rb_profile_begin ||
		eop >= layout->rb_profile_end ||
		data[eop] != 0x82)
	{
		return DC_STATUS_DATAFORMAT;
	}

	// Memory buffer for the profile ringbuffer.
	unsigned int length = layout->rb_profile_end - layout->rb_profile_begin;
	unsigned char *buffer = (unsigned char *) malloc (length);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	unsigned int current = eop;
	unsigned int previous = eop;
	for (unsigned int i = 0; i < length; ++i) {
		// Move backwards through the ringbuffer.
		if (current == layout->rb_profile_begin)
			current = layout->rb_profile_end;
		current--;

		// Check for an end of profile marker.
		if (data[current] == 0x82)
			break;

		// Check for an end of dive marker (of the next dive),
		// to find the start of the current dive.
		unsigned int idx = RB_PROFILE_PEEK (current, layout);
		if (data[idx] == 0x80) {
			unsigned int len = RB_PROFILE_DISTANCE (current, previous, layout);
			if (current + len > layout->rb_profile_end) {
				unsigned int a = layout->rb_profile_end - current;
				unsigned int b = (current + len) - layout->rb_profile_end;
				memcpy (buffer + 0, data + current, a);
				memcpy (buffer + a, data + layout->rb_profile_begin,   b);
			} else {
				memcpy (buffer, data + current, len);
			}

			if (device && memcmp (buffer + layout->fp_offset, device->fingerprint, sizeof (device->fingerprint)) == 0) {
				free (buffer);
				return DC_STATUS_SUCCESS;
			}

			if (callback && !callback (buffer, len, buffer + layout->fp_offset, sizeof (device->fingerprint), userdata)) {
				free (buffer);
				return DC_STATUS_SUCCESS;
			}

			previous = current;
		}
	}

	free (buffer);

	if (data[current] != 0x82)
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}
