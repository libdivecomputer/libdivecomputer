/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include <libdivecomputer/shearwater_predator.h>

#include "shearwater_common.h"

#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &shearwater_predator_device_vtable)

#define PREDATOR 2
#define PETREL   3

#define SZ_BLOCK   0x80
#define SZ_MEMORY  0x20080

#define RB_PROFILE_BEGIN 0
#define RB_PROFILE_END   0x1F600

typedef struct shearwater_predator_device_t {
	shearwater_common_device_t base;
	unsigned char fingerprint[4];
} shearwater_predator_device_t;

static dc_status_t shearwater_predator_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t shearwater_predator_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t shearwater_predator_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t shearwater_predator_device_close (dc_device_t *abstract);

static const dc_device_vtable_t shearwater_predator_device_vtable = {
	DC_FAMILY_SHEARWATER_PREDATOR,
	shearwater_predator_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	shearwater_predator_device_dump, /* dump */
	shearwater_predator_device_foreach, /* foreach */
	shearwater_predator_device_close /* close */
};


dc_status_t
shearwater_predator_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	shearwater_predator_device_t *device = (shearwater_predator_device_t *) malloc (sizeof (shearwater_predator_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base.base, context, &shearwater_predator_device_vtable);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	rc = shearwater_common_open (&device->base, context, name);
	if (rc != DC_STATUS_SUCCESS) {
		free (device);
		return rc;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_device_close (dc_device_t *abstract)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	shearwater_common_device_t *device = (shearwater_common_device_t *) abstract;

	// Close the device.
	rc = shearwater_common_close (device);

	// Free memory.
	free (device);

	return rc;
}


static dc_status_t
shearwater_predator_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	shearwater_predator_device_t *device = (shearwater_predator_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	shearwater_common_device_t *device = (shearwater_common_device_t *) abstract;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return shearwater_common_download (device, buffer, 0xDD000000, SZ_MEMORY, 0);
}


static dc_status_t
shearwater_predator_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = shearwater_predator_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = data[0x2000D];
	devinfo.firmware = data[0x2000A];
	devinfo.serial = array_uint32_le (data + 0x20002);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = shearwater_predator_extract_dives (abstract, data, SZ_MEMORY, callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
shearwater_predator_extract_predator (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	shearwater_predator_device_t *device = (shearwater_predator_device_t*) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Locate the most recent dive.
	// The device maintains an internal counter which is incremented for every
	// dive, and the current value at the time of the dive is stored in the
	// dive header. Thus the most recent dive will have the highest value.
	unsigned int maximum = 0;
	unsigned int eop = RB_PROFILE_END;

	// Search the ringbuffer backwards to locate matching header and
	// footer markers. Because the ringbuffer search algorithm starts at
	// some arbitrary position, which does not necessary corresponds
	// with a boundary between two dives, the begin position is adjusted
	// as soon as the first dive has been found. Without this step,
	// dives crossing the ringbuffer wrap point won't be detected when
	// searching backwards from the ringbuffer end offset.
	unsigned int footer = 0;
	unsigned int have_footer = 0;
	unsigned int begin = RB_PROFILE_BEGIN;
	unsigned int offset = RB_PROFILE_END;
	while (offset != begin) {
		// Handle the ringbuffer wrap point.
		if (offset == RB_PROFILE_BEGIN)
			offset = RB_PROFILE_END;

		// Move to the start of the block.
		offset -= SZ_BLOCK;

		if (array_isequal (data + offset, SZ_BLOCK, 0xFF)) {
			// Ignore empty blocks explicitly, because otherwise they are
			// incorrectly recognized as header markers.
		} else if (data[offset + 0] == 0xFF && data[offset + 1] == 0xFF && have_footer) {
			// If the first header marker is found, the begin offset is moved
			// after the corresponding footer marker. This is necessary to be
			// able to detect dives that cross the ringbuffer wrap point.
			if (begin == RB_PROFILE_BEGIN)
				begin = footer + SZ_BLOCK;

			// Get the internal dive number.
			unsigned int current = array_uint16_be (data + offset + 2);
			if (current > maximum) {
				maximum = current;
				eop = footer + SZ_BLOCK;
			}

			// The dive number in the header and footer should be identical.
			if (current != array_uint16_be (data + footer + 2)) {
				ERROR (context, "Unexpected dive number.");
				return DC_STATUS_DATAFORMAT;
			}

			// Reset the footer marker.
			have_footer = 0;
		} else if (data[offset + 0] == 0xFF && data[offset + 1] == 0xFE) {
			// Remember the footer marker.
			footer = offset;
			have_footer = 1;
		}
	}

	// Allocate memory for the profiles.
	unsigned char *buffer = (unsigned char *) malloc (RB_PROFILE_END - RB_PROFILE_BEGIN + SZ_BLOCK);
	if (buffer == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Linearize the ringbuffer.
	memcpy (buffer + 0, data + eop, RB_PROFILE_END - eop);
	memcpy (buffer + RB_PROFILE_END - eop, data + RB_PROFILE_BEGIN, eop - RB_PROFILE_BEGIN);

	// Find the dives again in the linear buffer.
	footer = 0;
	have_footer = 0;
	offset = RB_PROFILE_END;
	while (offset != RB_PROFILE_BEGIN) {
		// Move to the start of the block.
		offset -= SZ_BLOCK;

		if (array_isequal (buffer + offset, SZ_BLOCK, 0xFF)) {
			break;
		} else if (buffer[offset + 0] == 0xFF && buffer[offset + 1] == 0xFF && have_footer) {
			// Append the final block.
			unsigned int length = footer + SZ_BLOCK - offset;
			memcpy (buffer + offset + length, data + SZ_MEMORY - SZ_BLOCK, SZ_BLOCK);

			// Check the fingerprint data.
			if (device && memcmp (buffer + offset + 12, device->fingerprint, sizeof (device->fingerprint)) == 0)
				break;

			if (callback && !callback (buffer + offset, length + SZ_BLOCK, buffer + offset + 12, sizeof (device->fingerprint), userdata))
				break;

			have_footer = 0;
		} else if (buffer[offset + 0] == 0xFF && buffer[offset + 1] == 0xFE) {
			footer = offset;
			have_footer = 1;
		}
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_extract_petrel (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	shearwater_predator_device_t *device = (shearwater_predator_device_t*) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Allocate memory for the profiles.
	unsigned char *buffer = (unsigned char *) malloc (RB_PROFILE_END - RB_PROFILE_BEGIN + SZ_BLOCK);
	if (buffer == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Search the ringbuffer to locate matching header and footer
	// markers. Because the Petrel does reorder the internal ringbuffer
	// before sending the data, the most recent dive is always the first
	// one. Therefore, there is no need to search for it, as we have to
	// do for the Predator.
	unsigned int header = 0;
	unsigned int have_header = 0;
	unsigned int offset = RB_PROFILE_BEGIN;
	while (offset != RB_PROFILE_END) {
		if (array_isequal (data + offset, SZ_BLOCK, 0xFF)) {
			// Ignore empty blocks explicitly, because otherwise they are
			// incorrectly recognized as header markers.
			break;
		} else if (data[offset + 0] == 0xFF && data[offset + 1] == 0xFF) {
			// Remember the header marker.
			header = offset;
			have_header = 1;
		} else if (data[offset + 0] == 0xFF && data[offset + 1] == 0xFE && have_header) {
			// The dive number in the header and footer should be identical.
			if (memcmp (data + header + 2, data + offset + 2, 2) != 0) {
				ERROR (context, "Unexpected dive number.");
				free (buffer);
				return DC_STATUS_DATAFORMAT;
			}

			// Append the final block.
			unsigned int length = offset + SZ_BLOCK - header;
			memcpy (buffer, data + header, length);
			memcpy (buffer + length, data + SZ_MEMORY - SZ_BLOCK, SZ_BLOCK);

			// Check the fingerprint data.
			if (device && memcmp (buffer + 12, device->fingerprint, sizeof (device->fingerprint)) == 0)
				break;

			if (callback && !callback (buffer, length + SZ_BLOCK, buffer + 12, sizeof (device->fingerprint), userdata))
				break;

			// Reset the header marker.
			have_header = 0;
		}

		offset += SZ_BLOCK;
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	unsigned int model = data[0x2000D];

	if (model == PETREL) {
		return shearwater_predator_extract_petrel (abstract, data, size, callback, userdata);
	} else {
		return shearwater_predator_extract_predator (abstract, data, size, callback, userdata);
	}
}
