/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#include <stdlib.h>
#include <string.h>

#include "rbstream.h"
#include "context-private.h"
#include "device-private.h"

struct dc_rbstream_t {
	dc_device_t *device;
	dc_rbstream_direction_t direction;
	unsigned int pagesize;
	unsigned int packetsize;
	unsigned int begin;
	unsigned int end;
	unsigned int address;
	unsigned int offset;
	unsigned int available;
	unsigned int skip;
	unsigned char cache[];
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

dc_status_t
dc_rbstream_new (dc_rbstream_t **out, dc_device_t *device, unsigned int pagesize, unsigned int packetsize, unsigned int begin, unsigned int end, unsigned int address, dc_rbstream_direction_t direction)
{
	dc_rbstream_t *rbstream = NULL;

	if (out == NULL || device == NULL)
		return DC_STATUS_INVALIDARGS;

	// Page and packet size should be non-zero.
	if (pagesize == 0 || packetsize == 0) {
		ERROR (device->context, "Zero length page or packet size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Packet size should be a multiple of the page size.
	if (packetsize % pagesize != 0) {
		ERROR (device->context, "Packet size not a multiple of the page size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Ringbuffer boundaries should be aligned to the page size.
	if (begin % pagesize != 0 || end % pagesize != 0) {
		ERROR (device->context, "Ringbuffer not aligned to the page size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Ringbuffer boundaries should not be reversed.
	if (begin > end) {
		ERROR (device->context, "Ringbuffer boundaries reversed!");
		return DC_STATUS_INVALIDARGS;
	}

	// Packet size should be smaller than the ringbuffer size.
	if (packetsize > (end - begin)) {
		ERROR (device->context, "Packet size larger than the ringbuffer size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Address should be inside the ringbuffer.
	if (address < begin || address > end) {
		ERROR (device->context, "Address outside the ringbuffer!");
		return DC_STATUS_INVALIDARGS;
	}

	// Allocate memory.
	rbstream = (dc_rbstream_t *) malloc (sizeof(*rbstream) + packetsize);
	if (rbstream == NULL) {
		ERROR (device->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	rbstream->device = device;
	rbstream->direction = direction;
	rbstream->pagesize = pagesize;
	rbstream->packetsize = packetsize;
	rbstream->begin = begin;
	rbstream->end = end;
	if (direction == DC_RBSTREAM_FORWARD) {
		rbstream->address = ifloor(address, pagesize);
		rbstream->skip = address - rbstream->address;
	} else {
		rbstream->address = iceil(address, pagesize);
		rbstream->skip = rbstream->address - address;
	}
	rbstream->offset = 0;
	rbstream->available = 0;

	*out = rbstream;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_rbstream_read_backward (dc_rbstream_t *rbstream, dc_event_progress_t *progress, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int nbytes = 0;
	unsigned int offset = size;
	while (nbytes < size) {
		if (rbstream->available == 0) {
			// Handle the ringbuffer wrap point.
			if (rbstream->address == rbstream->begin)
				rbstream->address = rbstream->end;

			// Calculate the packet size.
			unsigned int len = rbstream->packetsize;
			if (rbstream->begin + len > rbstream->address)
				len = rbstream->address - rbstream->begin;

			// Read the packet into the cache.
			rc = dc_device_read (rbstream->device, rbstream->address - len, rbstream->cache, rbstream->packetsize);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Move to the end of the next packet.
			rbstream->address -= len;

			rbstream->available = len - rbstream->skip;
			rbstream->skip = 0;
		}

		unsigned int length = rbstream->available;
		if (nbytes + length > size)
			length = size - nbytes;

		offset -= length;
		rbstream->available -= length;

		memcpy (data + offset, rbstream->cache + rbstream->available, length);

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit (rbstream->device, DC_EVENT_PROGRESS, progress);
		}

		nbytes += length;
	}

	return rc;
}

static dc_status_t
dc_rbstream_read_forward (dc_rbstream_t *rbstream, dc_event_progress_t *progress, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		if (rbstream->available == 0) {
			// Handle the ringbuffer wrap point.
			if (rbstream->address == rbstream->end)
				rbstream->address = rbstream->begin;

			// Calculate the packet size.
			unsigned int len = rbstream->packetsize;
			if (rbstream->address + len > rbstream->end)
				len = rbstream->end - rbstream->address;

			// Calculate the excess number of bytes.
			unsigned int extra = rbstream->packetsize - len;

			// Read the packet into the cache.
			rc = dc_device_read (rbstream->device, rbstream->address - extra, rbstream->cache, rbstream->packetsize);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Move to the begin of the next packet.
			rbstream->address += len;

			rbstream->offset = extra + rbstream->skip;
			rbstream->available = len - rbstream->skip;
			rbstream->skip = 0;
		}

		unsigned int length = rbstream->available;
		if (nbytes + length > size)
			length = size - nbytes;

		memcpy (data + nbytes, rbstream->cache + rbstream->offset, length);

		rbstream->offset += length;
		rbstream->available -= length;

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit (rbstream->device, DC_EVENT_PROGRESS, progress);
		}

		nbytes += length;
	}

	return rc;
}

dc_status_t
dc_rbstream_read (dc_rbstream_t *rbstream, dc_event_progress_t *progress, unsigned char data[], unsigned int size)
{
	if (rbstream == NULL)
		return DC_STATUS_INVALIDARGS;

	if (rbstream->direction == DC_RBSTREAM_FORWARD) {
		return dc_rbstream_read_forward (rbstream, progress, data, size);
	} else {
		return dc_rbstream_read_backward (rbstream, progress, data, size);
	}
}

dc_status_t
dc_rbstream_free (dc_rbstream_t *rbstream)
{
	free (rbstream);

	return DC_STATUS_SUCCESS;
}
