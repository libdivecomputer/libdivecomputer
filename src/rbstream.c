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
	unsigned int pagesize;
	unsigned int packetsize;
	unsigned int begin;
	unsigned int end;
	unsigned int address;
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
dc_rbstream_new (dc_rbstream_t **out, dc_device_t *device, unsigned int pagesize, unsigned int packetsize, unsigned int begin, unsigned int end, unsigned int address)
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
	rbstream->pagesize = pagesize;
	rbstream->packetsize = packetsize;
	rbstream->begin = begin;
	rbstream->end = end;
	rbstream->address = iceil(address, pagesize);
	rbstream->available = 0;
	rbstream->skip = rbstream->address - address;

	*out = rbstream;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_rbstream_read (dc_rbstream_t *rbstream, dc_event_progress_t *progress, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	if (rbstream == NULL)
		return DC_STATUS_INVALIDARGS;

	unsigned int address = rbstream->address;
	unsigned int available = rbstream->available;
	unsigned int skip = rbstream->skip;

	unsigned int nbytes = 0;
	unsigned int offset = size;
	while (nbytes < size) {
		if (available == 0) {
			// Handle the ringbuffer wrap point.
			if (address == rbstream->begin)
				address = rbstream->end;

			// Calculate the packet size.
			unsigned int len = rbstream->packetsize;
			if (rbstream->begin + len > address)
				len = address - rbstream->begin;

			// Move to the begin of the current packet.
			address -= len;

			// Read the packet into the cache.
			rc = dc_device_read (rbstream->device, address, rbstream->cache, rbstream->packetsize);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			available = len - skip;
			skip = 0;
		}

		unsigned int length = available;
		if (nbytes + length > size)
			length = size - nbytes;

		offset -= length;
		available -= length;

		memcpy (data + offset, rbstream->cache + available, length);

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit (rbstream->device, DC_EVENT_PROGRESS, progress);
		}

		nbytes += length;
	}

	rbstream->address = address;
	rbstream->available = available;
	rbstream->skip = skip;

	return rc;
}

dc_status_t
dc_rbstream_free (dc_rbstream_t *rbstream)
{
	free (rbstream);

	return DC_STATUS_SUCCESS;
}
