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

#include "hdlc.h"

#include "iostream-private.h"
#include "common-private.h"
#include "context-private.h"

#define END     0x7E
#define ESC     0x7D
#define ESC_BIT 0x20

static dc_status_t dc_hdlc_set_timeout (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_hdlc_set_break (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_hdlc_set_dtr (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_hdlc_set_rts (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_hdlc_get_lines (dc_iostream_t *abstract, unsigned int *value);
static dc_status_t dc_hdlc_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_hdlc_poll (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_hdlc_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual);
static dc_status_t dc_hdlc_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual);
static dc_status_t dc_hdlc_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size);
static dc_status_t dc_hdlc_flush (dc_iostream_t *abstract);
static dc_status_t dc_hdlc_purge (dc_iostream_t *abstract, dc_direction_t direction);
static dc_status_t dc_hdlc_sleep (dc_iostream_t *abstract, unsigned int milliseconds);
static dc_status_t dc_hdlc_close (dc_iostream_t *abstract);

typedef struct dc_hdlc_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_context_t *context;
	dc_iostream_t *iostream;
	unsigned char *rbuf;
	unsigned char *wbuf;
	size_t rbuf_size;
	size_t rbuf_offset;
	size_t rbuf_available;
	size_t wbuf_size;
	size_t wbuf_offset;
} dc_hdlc_t;

static const dc_iostream_vtable_t dc_hdlc_vtable = {
	sizeof(dc_hdlc_t),
	dc_hdlc_set_timeout, /* set_timeout */
	dc_hdlc_set_break, /* set_break */
	dc_hdlc_set_dtr, /* set_dtr */
	dc_hdlc_set_rts, /* set_rts */
	dc_hdlc_get_lines, /* get_lines */
	NULL, /* get_available */
	dc_hdlc_configure, /* configure */
	dc_hdlc_poll, /* poll */
	dc_hdlc_read, /* read */
	dc_hdlc_write, /* write */
	dc_hdlc_ioctl, /* ioctl */
	dc_hdlc_flush, /* flush */
	dc_hdlc_purge, /* purge */
	dc_hdlc_sleep, /* sleep */
	dc_hdlc_close, /* close */
};

dc_status_t
dc_hdlc_open (dc_iostream_t **out, dc_context_t *context, dc_iostream_t *base, size_t isize, size_t osize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_hdlc_t *hdlc = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (base == NULL || isize == 0 || osize == 0)
		return DC_STATUS_INVALIDARGS;

	dc_transport_t transport = dc_iostream_get_transport (base);

	// Allocate memory.
	hdlc = (dc_hdlc_t *) dc_iostream_allocate (NULL, &dc_hdlc_vtable, transport);
	if (hdlc == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Allocate the read buffer.
	hdlc->rbuf = malloc (isize);
	if (hdlc->rbuf == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free;
	}

	// Allocate the write buffer.
	hdlc->wbuf = malloc (osize);
	if (hdlc->wbuf == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_rbuf;
	}

	hdlc->context = context;
	hdlc->iostream = base;
	hdlc->rbuf_size = isize;
	hdlc->rbuf_offset = 0;
	hdlc->rbuf_available = 0;
	hdlc->wbuf_size = osize;
	hdlc->wbuf_offset = 0;

	*out = (dc_iostream_t *) hdlc;

	return DC_STATUS_SUCCESS;

error_free_rbuf:
	free (hdlc->rbuf);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) hdlc);
error_exit:
	return status;
}

static dc_status_t
dc_hdlc_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_set_timeout (hdlc->iostream, timeout);
}

static dc_status_t
dc_hdlc_set_break (dc_iostream_t *abstract, unsigned int value)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_set_break (hdlc->iostream, value);
}

static dc_status_t
dc_hdlc_set_dtr (dc_iostream_t *abstract, unsigned int value)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_set_dtr (hdlc->iostream, value);
}

static dc_status_t
dc_hdlc_set_rts (dc_iostream_t *abstract, unsigned int value)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_set_rts (hdlc->iostream, value);
}

static dc_status_t
dc_hdlc_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_get_lines (hdlc->iostream, value);
}

static dc_status_t
dc_hdlc_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_configure (hdlc->iostream, baudrate, databits, parity, stopbits, flowcontrol);
}

static dc_status_t
dc_hdlc_poll (dc_iostream_t *abstract, int timeout)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	if (hdlc->rbuf_available) {
		return DC_STATUS_SUCCESS;
	}

	return dc_iostream_poll (hdlc->iostream, timeout);
}

static dc_status_t
dc_hdlc_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;
	size_t nbytes = 0;

	unsigned int initialized = 0;
	unsigned int escaped = 0;

	while (1) {
		if (hdlc->rbuf_available == 0) {
			// Read a packet into the cache.
			size_t len = 0;
			status = dc_iostream_read (hdlc->iostream, hdlc->rbuf, hdlc->rbuf_size, &len);
			if (status != DC_STATUS_SUCCESS) {
				goto out;
			}

			hdlc->rbuf_available = len;
			hdlc->rbuf_offset = 0;
		}

		while (hdlc->rbuf_available) {
			unsigned char c = hdlc->rbuf[hdlc->rbuf_offset];
			hdlc->rbuf_offset++;
			hdlc->rbuf_available--;

			if (c == END) {
				if (escaped) {
					ERROR (hdlc->context, "HDLC frame escaped the special character %02x.", c);
					status = DC_STATUS_IO;
					goto out;
				}

				if (initialized) {
					goto out;
				}

				initialized = 1;
				continue;
			}

			if (!initialized) {
				continue;
			}

			if (c == ESC) {
				if (escaped) {
					ERROR (hdlc->context, "HDLC frame escaped the special character %02x.", c);
					status = DC_STATUS_IO;
					goto out;
				}
				escaped = 1;
				continue;
			}

			if (escaped) {
				c ^= ESC_BIT;
				escaped = 0;
			}

			if (nbytes < size)
				((unsigned char *)data)[nbytes] = c;
			nbytes++;
		}
	}

out:
	if (nbytes > size) {
		ERROR (hdlc->context, "HDLC frame is too large (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", nbytes, size);
		dc_status_set_error (&status, DC_STATUS_IO);
		nbytes = size;
	}

	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_hdlc_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;
	size_t nbytes = 0;

	// Clear the buffer.
	hdlc->wbuf_offset = 0;

	// Start of the packet.
	hdlc->wbuf[hdlc->wbuf_offset++] = END;

	// Flush the buffer if necessary.
	if (hdlc->wbuf_offset >= hdlc->wbuf_size) {
		status = dc_iostream_write (hdlc->iostream, hdlc->wbuf, hdlc->wbuf_offset, NULL);
		if (status != DC_STATUS_SUCCESS) {
			goto out;
		}

		hdlc->wbuf_offset = 0;
	}

	while (nbytes < size) {
		unsigned char c = ((const unsigned char *) data)[nbytes];

		if (c == END || c == ESC) {
			// Append the escape character.
			hdlc->wbuf[hdlc->wbuf_offset++] = ESC;

			// Flush the buffer if necessary.
			if (hdlc->wbuf_offset >= hdlc->wbuf_size) {
				status = dc_iostream_write (hdlc->iostream, hdlc->wbuf, hdlc->wbuf_offset, NULL);
				if (status != DC_STATUS_SUCCESS) {
					goto out;
				}

				hdlc->wbuf_offset = 0;
			}

			// Escape the character.
			c ^= ESC_BIT;
		}

		// Append the character.
		hdlc->wbuf[hdlc->wbuf_offset++] = c;

		// Flush the buffer if necessary.
		if (hdlc->wbuf_offset >= hdlc->wbuf_size) {
			status = dc_iostream_write (hdlc->iostream, hdlc->wbuf, hdlc->wbuf_offset, NULL);
			if (status != DC_STATUS_SUCCESS) {
				goto out;
			}

			hdlc->wbuf_offset = 0;
		}

		nbytes++;
	}

	// End of the packet.
	hdlc->wbuf[hdlc->wbuf_offset++] = END;

	// Flush the buffer.
	status = dc_iostream_write (hdlc->iostream, hdlc->wbuf, hdlc->wbuf_offset, NULL);
	if (status != DC_STATUS_SUCCESS) {
		goto out;
	}

	hdlc->wbuf_offset = 0;

out:
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_hdlc_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_ioctl (hdlc->iostream, request, data, size);
}

static dc_status_t
dc_hdlc_flush (dc_iostream_t *abstract)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_flush (hdlc->iostream);
}

static dc_status_t
dc_hdlc_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	if (direction & DC_DIRECTION_INPUT) {
		hdlc->rbuf_available = 0;
		hdlc->rbuf_offset = 0;
	}

	return dc_iostream_purge (hdlc->iostream, direction);
}

static dc_status_t
dc_hdlc_sleep (dc_iostream_t *abstract, unsigned int milliseconds)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	return dc_iostream_sleep (hdlc->iostream, milliseconds);
}

static dc_status_t
dc_hdlc_close (dc_iostream_t *abstract)
{
	dc_hdlc_t *hdlc = (dc_hdlc_t *) abstract;

	free (hdlc->wbuf);
	free (hdlc->rbuf);

	return DC_STATUS_SUCCESS;
}
