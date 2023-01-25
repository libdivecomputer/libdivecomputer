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

#include <string.h> // memcpy, memmove
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "oceanic_common.h"
#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "rbstream.h"
#include "array.h"

#define VTABLE(abstract)	((const oceanic_common_device_vtable_t *) abstract->vtable)

#define RB_LOGBOOK_DISTANCE(a,b,l)	ringbuffer_distance (a, b, 1, l->rb_logbook_begin, l->rb_logbook_end)
#define RB_LOGBOOK_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_logbook_begin, l->rb_logbook_end)

#define RB_PROFILE_DISTANCE(a,b,l)	ringbuffer_distance (a, b, 0, l->rb_profile_begin, l->rb_profile_end)
#define RB_PROFILE_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_profile_begin, l->rb_profile_end)

#define INVALID 0

static unsigned int
get_profile_first (const unsigned char data[], const oceanic_common_layout_t *layout, unsigned int pagesize)
{
	unsigned int value;

	if (layout->pt_mode_logbook == 0) {
		value = array_uint16_le (data + 5);
	} else if (layout->pt_mode_logbook == 1) {
		value = array_uint16_le (data + 4);
	} else if (layout->pt_mode_logbook == 3) {
		value = array_uint16_le (data + 16);
	} else {
		return array_uint16_le (data + 16);
	}

	unsigned int npages = (layout->memsize - layout->highmem) / pagesize;
	if (npages > 0x4000) {
		value &= 0x7FFF;
	} else if (npages > 0x2000) {
		value &= 0x3FFF;
	}  else if (npages > 0x1000) {
		value &= 0x1FFF;
	} else {
		value &= 0x0FFF;
	}

	return layout->highmem + value * pagesize;
}


static unsigned int
get_profile_last (const unsigned char data[], const oceanic_common_layout_t *layout, unsigned int pagesize)
{
	unsigned int value;

	if (layout->pt_mode_logbook == 0) {
		value = array_uint16_le (data + 6) >> 4;
	} else if (layout->pt_mode_logbook == 1) {
		value = array_uint16_le (data + 6);
	} else if (layout->pt_mode_logbook == 3) {
		value = array_uint16_le (data + 18);
	} else {
		return array_uint16_le(data + 18);
	}

	unsigned int npages = (layout->memsize - layout->highmem) / pagesize;

	if (npages > 0x4000) {
		value &= 0x7FFF;
	} else if (npages > 0x2000) {
		value &= 0x3FFF;
	} else if (npages > 0x1000) {
		value &= 0x1FFF;
	} else {
		value &= 0x0FFF;
	}

	return layout->highmem + value * pagesize;
}


static int
oceanic_common_match_pattern (const unsigned char *string, const unsigned char *pattern, unsigned int *firmware)
{
	unsigned int value = 0;
	unsigned int count = 0;

	for (unsigned int i = 0; i < PAGESIZE; ++i, ++pattern, ++string) {
		if (*pattern != '\0') {
			// Compare the pattern.
			if (*pattern != *string)
				return 0;
		} else {
			// Extract the firmware version.
			// This is based on the assumption that (only) the first block of
			// zeros in the pattern contains the firmware version.
			if (i == 0 || *(pattern - 1) != '\0')
				count++;
			if (count == 1) {
				value <<= 8;
				value |= *string;
			}
		}
	}

	if (firmware) {
		*firmware = value;
	}

	return 1;
}

const oceanic_common_version_t *
oceanic_common_match (const unsigned char *version, const oceanic_common_version_t patterns[], size_t n, unsigned int *firmware)
{
	for (size_t i = 0; i < n; ++i) {
		unsigned int fw = 0;
		if (oceanic_common_match_pattern (version, patterns[i].pattern, &fw) &&
			fw >= patterns[i].firmware)
		{
			if (firmware) {
				*firmware = fw;
			}
			return patterns + i;
		}
	}

	return NULL;
}


void
oceanic_common_device_init (oceanic_common_device_t *device)
{
	assert (device != NULL);

	// Set the default values.
	device->firmware = 0;
	memset (device->version, 0, sizeof (device->version));
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->model = 0;
	device->layout = NULL;
	device->multipage = 1;
}


dc_status_t
oceanic_common_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);
	assert (device->layout->rb_logbook_entry_size <= sizeof (device->fingerprint));

	unsigned int fpsize = device->layout->rb_logbook_entry_size;

	if (size && size != fpsize)
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, fpsize);
	else
		memset (device->fingerprint, 0, fpsize);

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_common_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);

	const oceanic_common_layout_t *layout = device->layout;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Download the memory dump.
	status = device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), PAGESIZE * device->multipage);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Emit a device info event.
	unsigned char *id = dc_buffer_get_data (buffer) + layout->cf_devinfo;
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_be (id + 8);
	devinfo.firmware = device->firmware;
	if (layout->pt_mode_serial == 0)
		devinfo.serial = array_convert_bcd2dec (id + 10, 3);
	else if (layout->pt_mode_serial == 1)
		devinfo.serial = array_convert_bin2dec (id + 11, 3);
	else
		devinfo.serial =
			(id[11] & 0x0F) * 100000 + ((id[11] & 0xF0) >> 4) * 10000 +
			(id[12] & 0x0F) * 1000   + ((id[12] & 0xF0) >> 4) * 100 +
			(id[13] & 0x0F) * 10     + ((id[13] & 0xF0) >> 4) * 1;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return status;
}


dc_status_t
oceanic_common_device_logbook (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook)
{
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	assert (device != NULL);
	assert (device->layout != NULL);
	assert (device->layout->rb_logbook_entry_size <= sizeof (device->fingerprint));
	assert (progress != NULL);

	const oceanic_common_layout_t *layout = device->layout;

	// Erase the buffer.
	if (!dc_buffer_clear (logbook))
		return DC_STATUS_NOMEMORY;

	// For devices without a logbook ringbuffer, downloading dives isn't
	// possible. This is not considered a fatal error, but handled as if there
	// are no dives present.
	if (layout->rb_logbook_begin == layout->rb_logbook_end) {
		return DC_STATUS_SUCCESS;
	}

	// Read the pointer data.
	unsigned char pointers[PAGESIZE] = {0};
	rc = dc_device_read (abstract, layout->cf_pointers, pointers, sizeof (pointers));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory page.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int rb_logbook_first = array_uint16_le (pointers + 4);
	unsigned int rb_logbook_last  = array_uint16_le (pointers + 6);
	if (rb_logbook_last < layout->rb_logbook_begin ||
		rb_logbook_last >= layout->rb_logbook_end)
	{
		ERROR (abstract->context, "Invalid logbook end pointer detected (0x%04x).", rb_logbook_last);
		return DC_STATUS_DATAFORMAT;
	}

	// Calculate the end pointer.
	unsigned int rb_logbook_end = 0;
	if (layout->pt_mode_global == 0) {
		rb_logbook_end  = RB_LOGBOOK_INCR (rb_logbook_last, layout->rb_logbook_entry_size, layout);
	} else {
		rb_logbook_end  = rb_logbook_last;
	}

	// Calculate the number of bytes.
	// In a typical ringbuffer implementation with only two begin/end
	// pointers, there is no distinction possible between an empty and a
	// full ringbuffer. We always consider the ringbuffer full in that
	// case, because an empty ringbuffer can be detected by inspecting
	// the logbook entries once they are downloaded.
	unsigned int rb_logbook_size = 0;
	if (rb_logbook_first < layout->rb_logbook_begin ||
		rb_logbook_first >= layout->rb_logbook_end)
	{
		// Fall back to downloading the entire logbook ringbuffer as
		// workaround for an invalid logbook begin pointer!
		ERROR (abstract->context, "Invalid logbook begin pointer detected (0x%04x).", rb_logbook_first);
		rb_logbook_size = layout->rb_logbook_end - layout->rb_logbook_begin;
	} else {
		rb_logbook_size = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_end, layout);
	}

	// Update and emit a progress event.
	progress->current += PAGESIZE;
	progress->maximum += PAGESIZE;
	progress->maximum -= (layout->rb_logbook_end - layout->rb_logbook_begin) - rb_logbook_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	// Exit if there are no dives.
	if (rb_logbook_size == 0) {
		return DC_STATUS_SUCCESS;
	}

	// Allocate memory for the logbook entries.
	if (!dc_buffer_resize (logbook, rb_logbook_size))
		return DC_STATUS_NOMEMORY;

	// Cache the logbook pointer.
	unsigned char *logbooks = dc_buffer_get_data (logbook);

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, PAGESIZE, PAGESIZE * device->multipage, layout->rb_logbook_begin, layout->rb_logbook_end, rb_logbook_end);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// The logbook ringbuffer is read backwards to retrieve the most recent
	// entries first. If an already downloaded entry is identified (by means
	// of its fingerprint), the transfer is aborted immediately to reduce
	// the transfer time.
	unsigned int count = 0;
	unsigned int nbytes = 0;
	unsigned int offset = rb_logbook_size;
	while (nbytes < rb_logbook_size) {
		// Move to the start of the current entry.
		offset -= layout->rb_logbook_entry_size;

		// Read the logbook entry.
		rc = dc_rbstream_read (rbstream, progress, logbooks + offset, layout->rb_logbook_entry_size);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the memory.");
			dc_rbstream_free (rbstream);
			return rc;
		}

		nbytes += layout->rb_logbook_entry_size;

		// Check for uninitialized entries. Normally, such entries are
		// never present, except when the ringbuffer is actually empty,
		// but the ringbuffer pointers are not set to their empty values.
		// This appears to happen on some devices, and we attempt to
		// fix this here.
		if (array_isequal (logbooks + offset, layout->rb_logbook_entry_size, 0xFF)) {
			WARNING (abstract->context, "Uninitialized logbook entries detected!");
			continue;
		}

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (logbooks + offset, device->fingerprint, layout->rb_logbook_entry_size) == 0) {
			offset += layout->rb_logbook_entry_size;
			break;
		}

		count++;
	}

	// Update and emit a progress event.
	progress->maximum -= rb_logbook_size - nbytes;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	if (count) {
		dc_buffer_slice (logbook, offset, rb_logbook_size - offset);
	} else {
		dc_buffer_clear (logbook);
	}

	dc_rbstream_free (rbstream);

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_common_device_profile (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook, dc_dive_callback_t callback, void *userdata)
{
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_status_t rc = DC_STATUS_SUCCESS;

	assert (device != NULL);
	assert (device->layout != NULL);
	assert (device->layout->rb_logbook_entry_size <= sizeof (device->fingerprint));
	assert (progress != NULL);

	const oceanic_common_layout_t *layout = device->layout;

	// Get the pagesize
	unsigned int pagesize = layout->highmem ? 16 * PAGESIZE : PAGESIZE;

	// Cache the logbook pointer and size.
	const unsigned char *logbooks = dc_buffer_get_data (logbook);
	unsigned int rb_logbook_size = dc_buffer_get_size (logbook);

	// Go through the logbook entries a first time, to get the end of
	// profile pointer and calculate the total amount of bytes in the
	// profile ringbuffer.
	unsigned int rb_profile_end  = INVALID;
	unsigned int rb_profile_size = 0;

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	unsigned int remaining = layout->rb_profile_end - layout->rb_profile_begin;
	unsigned int previous = rb_profile_end;
	unsigned int entry = rb_logbook_size;
	while (entry) {
		// Move to the start of the current entry.
		entry -= layout->rb_logbook_entry_size;

		// Skip uninitialized entries.
		if (array_isequal (logbooks + entry, layout->rb_logbook_entry_size, 0xFF)) {
			WARNING (abstract->context, "Skipping uninitialized logbook entry!");
			continue;
		}

		// Get the profile pointers.
		unsigned int rb_entry_first = get_profile_first (logbooks + entry, layout, pagesize);
		unsigned int rb_entry_last  = get_profile_last (logbooks + entry, layout, pagesize);
		if (rb_entry_first < layout->rb_profile_begin ||
			rb_entry_first >= layout->rb_profile_end ||
			rb_entry_last < layout->rb_profile_begin ||
			rb_entry_last >= layout->rb_profile_end)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%06x 0x%06x).",
				rb_entry_first, rb_entry_last);
			status = DC_STATUS_DATAFORMAT;
			continue;
		}

		// Calculate the end pointer and the number of bytes.
		unsigned int rb_entry_end   = RB_PROFILE_INCR (rb_entry_last, pagesize, layout);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last, layout) + pagesize;

		// Take the end pointer of the most recent logbook entry as the
		// end of profile pointer.
		if (rb_profile_end == INVALID) {
			rb_profile_end = previous = rb_entry_end;
		}

		// Skip gaps between the profiles.
		unsigned int gap = 0;
		if (rb_entry_end != previous) {
			WARNING (abstract->context, "Profiles are not continuous.");
			gap = RB_PROFILE_DISTANCE (rb_entry_end, previous, layout);
		}

		// Make sure the profile size is valid.
		if (rb_entry_size + gap > remaining) {
			WARNING (abstract->context, "Unexpected profile size.");
			break;
		}

		// Update the total profile size.
		rb_profile_size += rb_entry_size + gap;

		remaining -= rb_entry_size + gap;
		previous = rb_entry_first;
	}

	// At this point, we know the exact amount of data
	// that needs to be transfered for the profiles.
	progress->maximum -= (layout->rb_profile_end - layout->rb_profile_begin) - rb_profile_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	// Exit if there are no dives.
	if (rb_profile_size == 0) {
		return status;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, PAGESIZE, PAGESIZE * device->multipage, layout->rb_profile_begin, layout->rb_profile_end, rb_profile_end);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// Memory buffer for the profile data.
	unsigned char *profiles = (unsigned char *) malloc (rb_profile_size + rb_logbook_size);
	if (profiles == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		dc_rbstream_free (rbstream);
		return DC_STATUS_NOMEMORY;
	}

	// Keep track of the current position.
	unsigned int offset = rb_profile_size + rb_logbook_size;

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	remaining = rb_profile_size;
	previous = rb_profile_end;
	entry = rb_logbook_size;
	while (entry) {
		// Move to the start of the current entry.
		entry -= layout->rb_logbook_entry_size;

		// Skip uninitialized entries.
		if (array_isequal (logbooks + entry, layout->rb_logbook_entry_size, 0xFF)) {
			WARNING (abstract->context, "Skipping uninitialized logbook entry!");
			continue;
		}

		// Get the profile pointers.
		unsigned int rb_entry_first = get_profile_first (logbooks + entry, layout, pagesize);
		unsigned int rb_entry_last  = get_profile_last (logbooks + entry, layout, pagesize);
		if (rb_entry_first < layout->rb_profile_begin ||
			rb_entry_first >= layout->rb_profile_end ||
			rb_entry_last < layout->rb_profile_begin ||
			rb_entry_last >= layout->rb_profile_end)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%06x 0x%06x).",
				rb_entry_first, rb_entry_last);
			status = DC_STATUS_DATAFORMAT;
			continue;
		}

		// Calculate the end pointer and the number of bytes.
		unsigned int rb_entry_end   = RB_PROFILE_INCR (rb_entry_last, pagesize, layout);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last, layout) + pagesize;

		// Skip gaps between the profiles.
		unsigned int gap = 0;
		if (rb_entry_end != previous) {
			WARNING (abstract->context, "Profiles are not continuous.");
			gap = RB_PROFILE_DISTANCE (rb_entry_end, previous, layout);
		}

		// Make sure the profile size is valid.
		if (rb_entry_size + gap > remaining) {
			WARNING (abstract->context, "Unexpected profile size.");
			break;
		}

		// Move to the start of the current dive.
		offset -= rb_entry_size + gap;

		// Read the dive.
		rc = dc_rbstream_read (rbstream, progress, profiles + offset, rb_entry_size + gap);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			status = rc;
			break;
		}

		remaining -= rb_entry_size + gap;
		previous = rb_entry_first;

		// Prepend the logbook entry to the profile data. The memory buffer is
		// large enough to store this entry.
		offset -= layout->rb_logbook_entry_size;
		memcpy (profiles + offset, logbooks + entry, layout->rb_logbook_entry_size);

		// Remove padding from the profile.
		if (layout->highmem) {
			// The logbook entry contains the total number of pages containing
			// profile data, excluding the footer page. Limit the profile size
			// to this size.
			unsigned int value = array_uint16_le (profiles + offset + 12);
			unsigned int value_hi = value & 0xE000;
			unsigned int value_lo = value & 0x0FFF;
			unsigned int npages = ((value_hi >> 1) | value_lo) + 1;
			unsigned int length = npages * PAGESIZE;
			if (rb_entry_size > length) {
				rb_entry_size = length;
			}
		}

		unsigned char *p = profiles + offset;
		if (callback && !callback (p, rb_entry_size + layout->rb_logbook_entry_size, p, layout->rb_logbook_entry_size, userdata)) {
			break;
		}
	}

	dc_rbstream_free (rbstream);
	free (profiles);

	return status;
}


dc_status_t
oceanic_common_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);

	const oceanic_common_layout_t *layout = device->layout;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = PAGESIZE +
		(layout->rb_logbook_end - layout->rb_logbook_begin) +
		(layout->rb_profile_end - layout->rb_profile_begin);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read the device id.
	unsigned char id[PAGESIZE] = {0};
	dc_status_t rc = dc_device_read (abstract, layout->cf_devinfo, id, sizeof (id));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory page.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += PAGESIZE;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_be (id + 8);
	devinfo.firmware = device->firmware;
	if (layout->pt_mode_serial == 0)
		devinfo.serial = array_convert_bcd2dec (id + 10, 3);
	else if (layout->pt_mode_serial == 1)
		devinfo.serial = array_convert_bin2dec (id + 11, 3);
	else
		devinfo.serial =
			(id[11] & 0x0F) * 100000 + ((id[11] & 0xF0) >> 4) * 10000 +
			(id[12] & 0x0F) * 1000   + ((id[12] & 0xF0) >> 4) * 100 +
			(id[13] & 0x0F) * 10     + ((id[13] & 0xF0) >> 4) * 1;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Memory buffer for the logbook data.
	dc_buffer_t *logbook = dc_buffer_new (0);
	if (logbook == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Download the logbook ringbuffer.
	rc = VTABLE(abstract)->logbook (abstract, &progress, logbook);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (logbook);
		return rc;
	}

	// Exit if there are no (new) dives.
	if (dc_buffer_get_size (logbook) == 0) {
		dc_buffer_free (logbook);
		return DC_STATUS_SUCCESS;
	}

	// Download the profile ringbuffer.
	rc = VTABLE(abstract)->profile (abstract, &progress, logbook, callback, userdata);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (logbook);
		return rc;
	}

	dc_buffer_free (logbook);

	return DC_STATUS_SUCCESS;
}
