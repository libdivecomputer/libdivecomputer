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
#include "array.h"

#define VTABLE(abstract)	((oceanic_common_device_vtable_t *) abstract->vtable)

#define RB_LOGBOOK_DISTANCE(a,b,l)	ringbuffer_distance (a, b, 0, l->rb_logbook_begin, l->rb_logbook_end)
#define RB_LOGBOOK_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_logbook_begin, l->rb_logbook_end)

#define RB_PROFILE_DISTANCE(a,b,l)	ringbuffer_distance (a, b, 0, l->rb_profile_begin, l->rb_profile_end)
#define RB_PROFILE_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_profile_begin, l->rb_profile_end)

#define INVALID 0

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


static unsigned int
get_profile_first (const unsigned char data[], const oceanic_common_layout_t *layout)
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

	if (layout->memsize > 0x20000)
		return (value & 0x3FFF) * PAGESIZE;
	else if (layout->memsize > 0x10000)
		return (value & 0x1FFF) * PAGESIZE;
	else
		return (value & 0x0FFF) * PAGESIZE;
}


static unsigned int
get_profile_last (const unsigned char data[], const oceanic_common_layout_t *layout)
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

	if (layout->memsize > 0x20000)
		return (value & 0x3FFF) * PAGESIZE;
	else if (layout->memsize > 0x10000)
		return (value & 0x1FFF) * PAGESIZE;
	else
		return (value & 0x0FFF) * PAGESIZE;
}


static int
oceanic_common_match_pattern (const unsigned char *string, const unsigned char *pattern)
{
	for (unsigned int i = 0; i < PAGESIZE; ++i, ++pattern, ++string) {
		if (*pattern != '\0' && *pattern != *string)
			return 0;
	}

	return 1;
}


int
oceanic_common_match (const unsigned char *version, const oceanic_common_version_t patterns[], unsigned int n)
{
	for (unsigned int i = 0; i < n; ++i) {
		if (oceanic_common_match_pattern (version, patterns[i]))
			return 1;
	}

	return 0;
}


void
oceanic_common_device_init (oceanic_common_device_t *device)
{
	assert (device != NULL);

	// Set the default values.
	memset (device->version, 0, sizeof (device->version));
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
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
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), PAGESIZE * device->multipage);
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
	if (rb_logbook_first < layout->rb_logbook_begin ||
		rb_logbook_first >= layout->rb_logbook_end ||
		rb_logbook_last < layout->rb_logbook_begin ||
		rb_logbook_last >= layout->rb_logbook_end)
	{
		ERROR (abstract->context, "Invalid logbook pointer detected (0x%04x 0x%04x).",
			rb_logbook_first, rb_logbook_last);
		return DC_STATUS_DATAFORMAT;
	}

	// Convert the first/last pointers to begin/end/count pointers.
	unsigned int rb_logbook_entry_begin, rb_logbook_entry_end,
		rb_logbook_entry_size;
	if (layout->pt_mode_global == 0) {
		rb_logbook_entry_begin = rb_logbook_first;
		rb_logbook_entry_end   = RB_LOGBOOK_INCR (rb_logbook_last, layout->rb_logbook_entry_size, layout);
		rb_logbook_entry_size  = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_last, layout) + layout->rb_logbook_entry_size;
	} else {
		rb_logbook_entry_begin = rb_logbook_first;
		rb_logbook_entry_end   = rb_logbook_last;
		rb_logbook_entry_size  = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_last, layout);
		// In a typical ringbuffer implementation with only two begin/end
		// pointers, there is no distinction possible between an empty and
		// a full ringbuffer. We always consider the ringbuffer full in
		// that case, because an empty ringbuffer can be detected by
		// inspecting the logbook entries once they are downloaded.
		if (rb_logbook_first == rb_logbook_last)
			rb_logbook_entry_size = layout->rb_logbook_end - layout->rb_logbook_begin;
	}

	// Check whether the ringbuffer is full.
	int full = (rb_logbook_entry_size == (layout->rb_logbook_end - layout->rb_logbook_begin));

	// Align the pointers to page boundaries.
	unsigned int rb_logbook_page_begin, rb_logbook_page_end,
		rb_logbook_page_size;
	if (full) {
		// Full ringbuffer.
		rb_logbook_page_begin = iceil (rb_logbook_entry_end, PAGESIZE);
		rb_logbook_page_end   = rb_logbook_page_begin;
		rb_logbook_page_size  = rb_logbook_entry_size;
	} else {
		// Non-full ringbuffer.
		rb_logbook_page_begin = ifloor (rb_logbook_entry_begin, PAGESIZE);
		rb_logbook_page_end   = iceil (rb_logbook_entry_end, PAGESIZE);
		rb_logbook_page_size  = rb_logbook_entry_size +
			(rb_logbook_entry_begin - rb_logbook_page_begin) +
			(rb_logbook_page_end - rb_logbook_entry_end);
	}

	// Check whether the last entry is not aligned to a page boundary.
	int unaligned = (rb_logbook_entry_end != rb_logbook_page_end);

	// Update and emit a progress event.
	progress->current += PAGESIZE;
	progress->maximum += PAGESIZE;
	progress->maximum -= (layout->rb_logbook_end - layout->rb_logbook_begin) - rb_logbook_page_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	// Exit if there are no dives.
	if (rb_logbook_page_size == 0) {
		return DC_STATUS_SUCCESS;
	}

	// Allocate memory for the logbook entries.
	if (!dc_buffer_resize (logbook, rb_logbook_page_size))
		return DC_STATUS_NOMEMORY;

	// Cache the logbook pointer.
	unsigned char *logbooks = dc_buffer_get_data (logbook);

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
	unsigned int nbytes = 0;
	unsigned int current = end;
	unsigned int offset = rb_logbook_page_size;
	unsigned int address = rb_logbook_page_end;
	while (nbytes < rb_logbook_page_size) {
		// Handle the ringbuffer wrap point.
		if (address == layout->rb_logbook_begin)
			address = layout->rb_logbook_end;

		// Calculate the optimal packet size.
		unsigned int len = PAGESIZE * device->multipage;
		if (layout->rb_logbook_begin + len > address)
			len = address - layout->rb_logbook_begin; // End of ringbuffer.
		if (nbytes + len > rb_logbook_page_size)
			len = rb_logbook_page_size - nbytes; // End of logbooks.

		// Move to the start of the current page.
		address -= len;
		offset -= len;

		// Read the logbook page.
		rc = dc_device_read (abstract, address, logbooks + offset, len);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the memory page.");
			return rc;
		}

		// Update and emit a progress event.
		progress->current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

		// A full ringbuffer needs some special treatment to avoid
		// having to download the first/last page twice. When a full
		// ringbuffer is not aligned to page boundaries, this page
		// will contain both the most recent and oldest entry.
		if (full && unaligned) {
			if (nbytes == 0) {
				// After downloading the first page, move both the oldest
				// and most recent entries to their correct location.
				unsigned int oldest = rb_logbook_page_end - rb_logbook_entry_end;
				unsigned int newest  = len - oldest;
				// Move the oldest entries down to the start of the buffer.
				memcpy (logbooks, logbooks + offset + newest, oldest);
				// Move the newest entries up to the end of the buffer.
				memmove (logbooks + offset + oldest, logbooks + offset, newest);
				// Adjust the current page offset to the new position.
				offset += oldest;
			} else if (nbytes + len == rb_logbook_page_size) {
				// After downloading the last page, pretend we have also
				// downloaded those oldest entries from the first page.
				offset = 0;
			}
		}

		nbytes += len;

		// Process the logbook entries.
		int abort = 0;
		while (current >= offset + layout->rb_logbook_entry_size &&
			current != offset && current != begin)
		{
			// Move to the start of the current entry.
			current -= layout->rb_logbook_entry_size;

			// Check for uninitialized entries. Normally, such entries are
			// never present, except when the ringbuffer is actually empty,
			// but the ringbuffer pointers are not set to their empty values.
			// This appears to happen on some devices, and we attempt to
			// fix this here.
			if (array_isequal (logbooks + current, layout->rb_logbook_entry_size, 0xFF)) {
				WARNING (abstract->context, "Uninitialized logbook entries detected!");
				begin = current + layout->rb_logbook_entry_size;
				abort = 1;
				break;
			}

			// Compare the fingerprint to identify previously downloaded entries.
			if (memcmp (logbooks + current, device->fingerprint, layout->rb_logbook_entry_size) == 0) {
				begin = current + layout->rb_logbook_entry_size;
				abort = 1;
				break;
			}
		}

		// Stop reading pages too.
		if (abort)
			break;
	}

	// Update and emit a progress event.
	progress->maximum -= rb_logbook_page_size - nbytes;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	dc_buffer_slice (logbook, begin, end - begin);

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_common_device_profile (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook, dc_dive_callback_t callback, void *userdata)
{
	oceanic_common_device_t *device = (oceanic_common_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	assert (device != NULL);
	assert (device->layout != NULL);
	assert (device->layout->rb_logbook_entry_size <= sizeof (device->fingerprint));
	assert (progress != NULL);

	const oceanic_common_layout_t *layout = device->layout;

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

		// Get the profile pointers.
		unsigned int rb_entry_first = get_profile_first (logbooks + entry, layout);
		unsigned int rb_entry_last  = get_profile_last (logbooks + entry, layout);
		if (rb_entry_first < layout->rb_profile_begin ||
			rb_entry_first >= layout->rb_profile_end ||
			rb_entry_last < layout->rb_profile_begin ||
			rb_entry_last >= layout->rb_profile_end)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%06x 0x%06x).",
				rb_entry_first, rb_entry_last);
			break;
		}

		// Calculate the end pointer and the number of bytes.
		unsigned int rb_entry_end   = RB_PROFILE_INCR (rb_entry_last, PAGESIZE, layout);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last, layout) + PAGESIZE;

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

	// Memory buffer for the profile data.
	unsigned char *profiles = (unsigned char *) malloc (rb_profile_size + rb_logbook_size);
	if (profiles == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Keep track of the current position.
	unsigned int offset = rb_profile_size + rb_logbook_size;
	unsigned int address = rb_profile_end;

	// When using multipage reads, the last packet can contain data from more
	// than one dive. Therefore, the remaining data of this package (and its
	// size) needs to be preserved for the next dive.
	unsigned int available = 0;

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

		// Get the profile pointers.
		unsigned int rb_entry_first = get_profile_first (logbooks + entry, layout);
		unsigned int rb_entry_last  = get_profile_last (logbooks + entry, layout);
		if (rb_entry_first < layout->rb_profile_begin ||
			rb_entry_first >= layout->rb_profile_end ||
			rb_entry_last < layout->rb_profile_begin ||
			rb_entry_last >= layout->rb_profile_end)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%06x 0x%06x).",
				rb_entry_first, rb_entry_last);
			free (profiles);
			return DC_STATUS_DATAFORMAT;
		}

		// Calculate the end pointer and the number of bytes.
		unsigned int rb_entry_end   = RB_PROFILE_INCR (rb_entry_last, PAGESIZE, layout);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last, layout) + PAGESIZE;

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

		// Read the profile data.
		unsigned int nbytes = available;
		while (nbytes < rb_entry_size + gap) {
			// Handle the ringbuffer wrap point.
			if (address == layout->rb_profile_begin)
				address = layout->rb_profile_end;

			// Calculate the optimal packet size.
			unsigned int len = PAGESIZE * device->multipage;
			if (layout->rb_profile_begin + len > address)
				len = address - layout->rb_profile_begin; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.

			// Move to the start of the current page.
			address -= len;
			offset -= len;

			// Read the profile page.
			rc = dc_device_read (abstract, address, profiles + offset, len);
			if (rc != DC_STATUS_SUCCESS) {
				free (profiles);
				return rc;
			}

			// Update and emit a progress event.
			progress->current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

			nbytes += len;
		}

		available = nbytes - (rb_entry_size + gap);
		remaining -= rb_entry_size + gap;
		previous = rb_entry_first;

		// Prepend the logbook entry to the profile data. The memory buffer is
		// large enough to store this entry, but any data that belongs to the
		// next dive needs to be moved down first.
		if (available)
			memmove (profiles + offset - layout->rb_logbook_entry_size, profiles + offset, available);
		offset -= layout->rb_logbook_entry_size;
		memcpy (profiles + offset + available, logbooks + entry, layout->rb_logbook_entry_size);

		unsigned char *p = profiles + offset + available;
		if (callback && !callback (p, rb_entry_size + layout->rb_logbook_entry_size, p, layout->rb_logbook_entry_size, userdata)) {
			break;
		}
	}

	free (profiles);

	return DC_STATUS_SUCCESS;
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
	devinfo.firmware = 0;
	if (layout->pt_mode_serial == 0)
		devinfo.serial = bcd2dec (id[10]) * 10000 + bcd2dec (id[11]) * 100 + bcd2dec (id[12]);
	else if (layout->pt_mode_serial == 1)
		devinfo.serial = id[11] * 10000 + id[12] * 100 + id[13];
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
