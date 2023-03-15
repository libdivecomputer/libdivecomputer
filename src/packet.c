/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Jef Driesen
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

#include <stdlib.h> // malloc, free
#include <string.h>

#include "packet.h"

#include "iostream-private.h"
#include "common-private.h"
#include "context-private.h"

static dc_status_t dc_packet_set_timeout (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_packet_set_break (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_packet_set_dtr (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_packet_set_rts (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_packet_get_lines (dc_iostream_t *abstract, unsigned int *value);
static dc_status_t dc_packet_get_available (dc_iostream_t *abstract, size_t *value);
static dc_status_t dc_packet_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_packet_poll (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_packet_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual);
static dc_status_t dc_packet_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual);
static dc_status_t dc_packet_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size);
static dc_status_t dc_packet_flush (dc_iostream_t *abstract);
static dc_status_t dc_packet_purge (dc_iostream_t *abstract, dc_direction_t direction);
static dc_status_t dc_packet_sleep (dc_iostream_t *abstract, unsigned int milliseconds);
static dc_status_t dc_packet_close (dc_iostream_t *abstract);

typedef struct dc_packet_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_iostream_t *iostream;
	unsigned char *cache;
	size_t available;
	size_t offset;
	size_t isize;
	size_t osize;
} dc_packet_t;

static const dc_iostream_vtable_t dc_packet_vtable = {
	sizeof(dc_packet_t),
	dc_packet_set_timeout, /* set_timeout */
	dc_packet_set_break, /* set_break */
	dc_packet_set_dtr, /* set_dtr */
	dc_packet_set_rts, /* set_rts */
	dc_packet_get_lines, /* get_lines */
	dc_packet_get_available, /* get_available */
	dc_packet_configure, /* configure */
	dc_packet_poll, /* poll */
	dc_packet_read, /* read */
	dc_packet_write, /* write */
	dc_packet_ioctl, /* ioctl */
	dc_packet_flush, /* flush */
	dc_packet_purge, /* purge */
	dc_packet_sleep, /* sleep */
	dc_packet_close, /* close */
};

dc_status_t
dc_packet_open (dc_iostream_t **out, dc_context_t *context, dc_iostream_t *base, size_t isize, size_t osize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_packet_t *packet = NULL;
	unsigned char *buffer = NULL;

	if (out == NULL || base == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	packet = (dc_packet_t *) dc_iostream_allocate (NULL, &dc_packet_vtable, dc_iostream_get_transport(base));
	if (packet == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Allocate the read buffer.
	if (isize) {
		buffer = (unsigned char *) malloc (isize);
		if (buffer == NULL) {
			ERROR (context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_free;
		}
	}

	packet->iostream = base;
	packet->cache = buffer;
	packet->available = 0;
	packet->offset = 0;
	packet->isize = isize;
	packet->osize = osize;

	*out = (dc_iostream_t *) packet;

	return DC_STATUS_SUCCESS;

error_free:
	dc_iostream_deallocate ((dc_iostream_t *) packet);
error_exit:
	return status;
}

static dc_status_t
dc_packet_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_set_timeout (packet->iostream, timeout);
}

static dc_status_t
dc_packet_set_break (dc_iostream_t *abstract, unsigned int value)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_set_break (packet->iostream, value);
}

static dc_status_t
dc_packet_set_dtr (dc_iostream_t *abstract, unsigned int value)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_set_dtr (packet->iostream, value);
}

static dc_status_t
dc_packet_set_rts (dc_iostream_t *abstract, unsigned int value)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_set_rts (packet->iostream, value);
}

static dc_status_t
dc_packet_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_get_lines (packet->iostream, value);
}

static dc_status_t
dc_packet_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	if (packet->isize && packet->available) {
		if (value)
			*value = packet->available;
		return DC_STATUS_SUCCESS;
	}

	return dc_iostream_get_available (packet->iostream, value);
}

static dc_status_t
dc_packet_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_configure (packet->iostream, baudrate, databits, parity, stopbits, flowcontrol);
}

static dc_status_t
dc_packet_poll (dc_iostream_t *abstract, int timeout)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	if (packet->isize && packet->available)
		return DC_STATUS_SUCCESS;

	return dc_iostream_poll (packet->iostream, timeout);
}

static dc_status_t
dc_packet_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_packet_t *packet = (dc_packet_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		// Get the remaining size.
		size_t length = size - nbytes;

		if (packet->isize) {
			if (packet->available == 0) {
				// Read a packet into the cache.
				size_t len = 0;
				status = dc_iostream_read (packet->iostream, packet->cache, packet->isize, &len);
				if (status != DC_STATUS_SUCCESS)
					break;

				packet->available = len;
				packet->offset = 0;
			}

			// Limit to the maximum packet size.
			if (length > packet->available)
				length = packet->available;

			// Copy the data from the cached packet.
			memcpy ((unsigned char *) data + nbytes, packet->cache + packet->offset, length);
			packet->available -= length;
			packet->offset += length;
		} else {
			// Read the packet.
			status = dc_iostream_read (packet->iostream, (unsigned char *) data + nbytes, length, &length);
			if (status != DC_STATUS_SUCCESS)
				break;
		}

		// Update the total number of bytes.
		nbytes += length;
	}

	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_packet_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_packet_t *packet = (dc_packet_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		// Get the remaining size.
		size_t length = size - nbytes;

		// Limit to the maximum packet size.
		if (packet->osize) {
			if (length > packet->osize)
				length = packet->osize;
		}

		// Write the packet.
		status = dc_iostream_write (packet->iostream, (const unsigned char *) data + nbytes, length, &length);
		if (status != DC_STATUS_SUCCESS)
			break;

		// Update the total number of bytes.
		nbytes += length;
	}

	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_packet_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_ioctl (packet->iostream, request, data, size);
}

static dc_status_t
dc_packet_flush (dc_iostream_t *abstract)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_flush (packet->iostream);
}

static dc_status_t
dc_packet_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	if (direction & DC_DIRECTION_INPUT) {
		packet->available = 0;
		packet->offset = 0;
	}

	return dc_iostream_purge (packet->iostream, direction);
}

static dc_status_t
dc_packet_sleep (dc_iostream_t *abstract, unsigned int milliseconds)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	return dc_iostream_sleep (packet->iostream, milliseconds);
}

static dc_status_t
dc_packet_close (dc_iostream_t *abstract)
{
	dc_packet_t *packet = (dc_packet_t *) abstract;

	free (packet->cache);

	return DC_STATUS_SUCCESS;
}
