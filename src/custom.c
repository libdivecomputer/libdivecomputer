/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#include <libdivecomputer/custom.h>

#include "iostream-private.h"
#include "common-private.h"
#include "context-private.h"

static dc_status_t dc_custom_set_timeout (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_custom_set_break (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_custom_set_dtr (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_custom_set_rts (dc_iostream_t *abstract, unsigned int value);
static dc_status_t dc_custom_get_lines (dc_iostream_t *abstract, unsigned int *value);
static dc_status_t dc_custom_get_available (dc_iostream_t *abstract, size_t *value);
static dc_status_t dc_custom_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_custom_poll (dc_iostream_t *abstract, int timeout);
static dc_status_t dc_custom_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual);
static dc_status_t dc_custom_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual);
static dc_status_t dc_custom_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size);
static dc_status_t dc_custom_flush (dc_iostream_t *abstract);
static dc_status_t dc_custom_purge (dc_iostream_t *abstract, dc_direction_t direction);
static dc_status_t dc_custom_sleep (dc_iostream_t *abstract, unsigned int milliseconds);
static dc_status_t dc_custom_close (dc_iostream_t *abstract);

typedef struct dc_custom_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_custom_cbs_t callbacks;
	void *userdata;
} dc_custom_t;

static const dc_iostream_vtable_t dc_custom_vtable = {
	sizeof(dc_custom_t),
	dc_custom_set_timeout, /* set_timeout */
	dc_custom_set_break, /* set_break */
	dc_custom_set_dtr, /* set_dtr */
	dc_custom_set_rts, /* set_rts */
	dc_custom_get_lines, /* get_lines */
	dc_custom_get_available, /* get_available */
	dc_custom_configure, /* configure */
	dc_custom_poll, /* poll */
	dc_custom_read, /* read */
	dc_custom_write, /* write */
	dc_custom_ioctl, /* ioctl */
	dc_custom_flush, /* flush */
	dc_custom_purge, /* purge */
	dc_custom_sleep, /* sleep */
	dc_custom_close, /* close */
};

dc_status_t
dc_custom_open (dc_iostream_t **out, dc_context_t *context, dc_transport_t transport, const dc_custom_cbs_t *callbacks, void *userdata)
{
	dc_custom_t *custom = NULL;

	if (out == NULL || callbacks == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: transport=%u", transport);

	// Allocate memory.
	custom = (dc_custom_t *) dc_iostream_allocate (context, &dc_custom_vtable, transport);
	if (custom == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	custom->callbacks = *callbacks;
	custom->userdata = userdata;

	*out = (dc_iostream_t *) custom;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_custom_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.set_timeout == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.set_timeout (custom->userdata, timeout);
}

static dc_status_t
dc_custom_set_break (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.set_break == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.set_break (custom->userdata, value);
}

static dc_status_t
dc_custom_set_dtr (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.set_dtr == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.set_dtr (custom->userdata, value);
}

static dc_status_t
dc_custom_set_rts (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.set_rts == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.set_rts (custom->userdata, value);
}

static dc_status_t
dc_custom_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.get_lines == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.get_lines (custom->userdata, value);
}

static dc_status_t
dc_custom_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.get_available == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.get_available (custom->userdata, value);
}

static dc_status_t
dc_custom_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.configure == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.configure (custom->userdata, baudrate, databits, parity, stopbits, flowcontrol);
}

static dc_status_t
dc_custom_poll (dc_iostream_t *abstract, int timeout)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.poll == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.poll (custom->userdata, timeout);
}

static dc_status_t
dc_custom_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.read == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.read (custom->userdata, data, size, actual);
}

static dc_status_t
dc_custom_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.write == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.write (custom->userdata, data, size, actual);
}

static dc_status_t
dc_custom_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.ioctl == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.ioctl (custom->userdata, request, data, size);
}

static dc_status_t
dc_custom_flush (dc_iostream_t *abstract)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.flush == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.flush (custom->userdata);
}

static dc_status_t
dc_custom_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.purge == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.purge (custom->userdata, direction);
}

static dc_status_t
dc_custom_sleep (dc_iostream_t *abstract, unsigned int milliseconds)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.sleep == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.sleep (custom->userdata, milliseconds);
}

static dc_status_t
dc_custom_close (dc_iostream_t *abstract)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;

	if (custom->callbacks.close == NULL)
		return DC_STATUS_SUCCESS;

	return custom->callbacks.close (custom->userdata);
}
