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
#include "device-private.h"
#include "ringbuffer.h"
#include "array.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define PAGESIZE 0x10

#define RB_LOGBOOK_DISTANCE(a,b,l)	ringbuffer_distance (a, b, l->rb_logbook_begin, l->rb_logbook_end)
#define RB_LOGBOOK_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_logbook_begin, l->rb_logbook_end)

#define RB_PROFILE_DISTANCE(a,b,l)	ringbuffer_distance (a, b, l->rb_profile_begin, l->rb_profile_end)
#define RB_PROFILE_INCR(a,b,l)		ringbuffer_increment (a, b, l->rb_profile_begin, l->rb_profile_end)


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


static unsigned char
bcd (unsigned char value)
{
	unsigned char lower = (value     ) & 0x0F;
	unsigned char upper = (value >> 4) & 0x0F;

	return lower + 10 * upper;
}


static unsigned int
get_profile_first (const unsigned char data[], const oceanic_common_layout_t *layout)
{
	unsigned int value;

	if (layout->mode == 0) {
		value = array_uint16_le (data + 5);
	} else {
		value = array_uint16_le (data + 4);
	}

	return (value & 0x0FFF) * PAGESIZE;
}


static unsigned int
get_profile_last (const unsigned char data[], const oceanic_common_layout_t *layout)
{
	unsigned int value;

	if (layout->mode == 0) {
		value = array_uint16_le (data + 6) >> 4;
	} else {
		value = array_uint16_le (data + 6);
	}

	return (value & 0x0FFF) * PAGESIZE;
}


void
oceanic_common_device_init (oceanic_common_device_t *device, const device_backend_t *backend)
{
	assert (device != NULL);

	// Initialize the base class.
	device_init (&device->base, backend);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
}


device_status_t
oceanic_common_device_set_fingerprint (oceanic_common_device_t *device, const unsigned char data[], unsigned int size)
{
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
oceanic_common_device_foreach (oceanic_common_device_t *device, const oceanic_common_layout_t *layout, dive_callback_t callback, void *userdata)
{
	device_t *abstract = (device_t *) device;

	assert (abstract != NULL);
	assert (layout != NULL);

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = 2 * PAGESIZE +
		(layout->rb_profile_end - layout->rb_profile_begin) +
		(layout->rb_logbook_end - layout->rb_logbook_begin);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Read the device id.
	unsigned char id[PAGESIZE] = {0};
	device_status_t rc = device_read (abstract, layout->cf_devinfo, id, sizeof (id));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read device id.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += PAGESIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = array_uint16_be (id + 8);
	devinfo.firmware = 0;
	devinfo.serial = bcd (id[10]) * 10000 + bcd (id[11]) * 100 + bcd (id[12]);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	// Read the pointer data.
	unsigned char pointers[PAGESIZE] = {0};
	rc = device_read (abstract, layout->cf_pointers, pointers, sizeof (pointers));
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
	if (rb_logbook_first == layout->rb_logbook_empty &&
		rb_logbook_last == layout->rb_logbook_empty)
	{
		// Empty ringbuffer.
		rb_logbook_entry_begin = layout->rb_logbook_begin;
		rb_logbook_entry_end   = layout->rb_logbook_begin;
		rb_logbook_entry_size  = 0;
	} else {
		// Non-empty ringbuffer.
		if (layout->mode == 0) {
			rb_logbook_entry_begin = rb_logbook_first;
			rb_logbook_entry_end   = RB_LOGBOOK_INCR (rb_logbook_last, PAGESIZE / 2, layout);
			rb_logbook_entry_size  = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_last, layout) + PAGESIZE / 2;
		} else {
			rb_logbook_entry_begin = rb_logbook_first;
			rb_logbook_entry_end   = rb_logbook_last;
			rb_logbook_entry_size  = RB_LOGBOOK_DISTANCE (rb_logbook_first, rb_logbook_last, layout);
			// In a typical ringbuffer implementation with only two begin/end
			// pointers, there is no distinction possible between an empty and
			// a full ringbuffer. Fortunately, the empty ringbuffer is stored
			// differently, and we can detect the difference correctly.
			if (rb_logbook_first == rb_logbook_last)
				rb_logbook_entry_size = layout->rb_logbook_end - layout->rb_logbook_begin;
		}
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
	progress.current += PAGESIZE;
	progress.maximum = 2 * PAGESIZE +
		(layout->rb_profile_end - layout->rb_profile_begin) +
		rb_logbook_page_size;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Memory buffer for the logbook entries.
	unsigned char *logbooks = (unsigned char *) malloc (rb_logbook_page_size);
	if (logbooks == NULL)
		return DEVICE_STATUS_MEMORY;

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
	unsigned int current = end;
	unsigned int offset = rb_logbook_page_size;
	unsigned int address = rb_logbook_page_end;
	unsigned int npages = rb_logbook_page_size / PAGESIZE;
	for (unsigned int i = 0; i < npages; ++i) {
		// Move to the start of the current page.
		if (address == layout->rb_logbook_begin)
			address = layout->rb_logbook_end;
		address -= PAGESIZE;
		offset -= PAGESIZE;

		// Read the logbook page.
		rc = device_read (abstract, address, logbooks + offset, PAGESIZE);
		if (rc != DEVICE_STATUS_SUCCESS) {
			free (logbooks);
			return rc;
		}

		// Update and emit a progress event.
		progress.current += PAGESIZE;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		// A full ringbuffer needs some special treatment to avoid
		// having to download the first/last page twice. When a full
		// ringbuffer is not aligned to page boundaries, this page
		// will contain both the most recent and oldest entry.
		if (full && unaligned) {
			if (i == 0) {
				// After downloading the first page, move both the oldest
				// and most recent entries to their correct location.
				unsigned int oldest = rb_logbook_page_end - rb_logbook_entry_end;
				unsigned int newest  = PAGESIZE - oldest;
				// Move the oldest entries down to the start of the buffer.
				memcpy (logbooks, logbooks + offset + newest, oldest);
				// Move the newest entries up to the end of the buffer.
				memmove (logbooks + offset + oldest, logbooks + offset, newest);
				// Adjust the current page offset to the new position.
				offset += oldest;
			} else if (i == npages - 1) {
				// After downloading the last page, pretend we have also
				// downloaded those oldest entries from the first page.
				offset = 0;
			}
		}

		// Process the logbook entries.
		int abort = 0;
		while (current != offset && current != begin) {
			// Move to the start of the current entry.
			current -= PAGESIZE / 2;

			// Compare the fingerprint to identify previously downloaded entries.
			if (memcmp (logbooks + current, device->fingerprint, PAGESIZE / 2) == 0) {
				begin = current + PAGESIZE / 2;
				abort = 1;
				break;
			}
		}

		// Stop reading pages too.
		if (abort)
			break;
	}

	// Exit if there are no (new) dives.
	if (begin == end) {
		free (logbooks);
		return DEVICE_STATUS_SUCCESS;
	}

	// Calculate the total amount of bytes in the profile ringbuffer,
	// based on the pointers in the first and last logbook entry.
	unsigned int rb_profile_first = get_profile_first (logbooks + begin, layout);
	unsigned int rb_profile_last  = get_profile_last (logbooks + end - PAGESIZE / 2, layout);
	unsigned int rb_profile_end   = RB_PROFILE_INCR (rb_profile_last, PAGESIZE, layout);
	unsigned int rb_profile_size  = RB_PROFILE_DISTANCE (rb_profile_first, rb_profile_last, layout) + PAGESIZE;

	// At this point, we know the exact amount of data
	// that needs to be transfered for the profiles.
	progress.maximum = progress.current + rb_profile_size;

	// Memory buffer for the profile data.
	unsigned char *profiles = (unsigned char *) malloc (rb_profile_size + PAGESIZE / 2);
	if (profiles == NULL) {
		free (logbooks);
		return DEVICE_STATUS_MEMORY;
	}

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	current = end;
	offset = rb_profile_size + PAGESIZE / 2;
	address = rb_profile_end;
	while (current != begin) {
		// Move to the start of the current entry.
		current -= PAGESIZE / 2;

		// Get the profile pointers.
		unsigned int rb_entry_first = get_profile_first (logbooks + current, layout);
		unsigned int rb_entry_last  = get_profile_last (logbooks + current, layout);
		unsigned int rb_entry_end   = RB_PROFILE_INCR (rb_entry_last, PAGESIZE, layout);
		unsigned int rb_entry_size  = RB_PROFILE_DISTANCE (rb_entry_first, rb_entry_last, layout) + PAGESIZE;

		// Make sure the profiles are continuous.
		assert (address == rb_entry_end);

		// Read the profile data.
		npages = rb_entry_size / PAGESIZE;
		for (unsigned int i = 0; i < npages; ++i) {
			// Move to the start of the current page.
			if (address == layout->rb_profile_begin)
				address = layout->rb_profile_end;
			address -= PAGESIZE;
			offset -= PAGESIZE;

			// Read the profile page.
			rc = device_read (abstract, address, profiles + offset, PAGESIZE);
			if (rc != DEVICE_STATUS_SUCCESS) {
				free (logbooks);
				free (profiles);
				return rc;
			}

			// Update and emit a progress event.
			progress.current += PAGESIZE;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);
		}

		// Prepend the logbook entry to the profile data. The memory buffer
		// is large enough to store this entry, but it will be overwritten
		// when the next profile is downloaded.
		memcpy (profiles + offset - PAGESIZE / 2, logbooks + current, PAGESIZE / 2);

		if (callback && !callback (profiles + offset - PAGESIZE / 2, rb_entry_size + PAGESIZE / 2, userdata)) {
			free (logbooks);
			free (profiles);
			return DEVICE_STATUS_SUCCESS;
		}
	}

	free (logbooks);
	free (profiles);

	return DEVICE_STATUS_SUCCESS;
}
