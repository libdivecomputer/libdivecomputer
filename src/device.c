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

#include <assert.h>
#include <stdlib.h>

#include "device-private.h"


void
device_init (device_t *device, const device_backend_t *backend)
{
	device->backend = backend;

	device->event_mask = 0;
	device->event_callback = NULL;
	device->event_userdata = NULL;

	device->cancel_callback = NULL;
	device->cancel_userdata = NULL;
}


device_type_t
device_get_type (device_t *device)
{
	if (device == NULL)
		return DEVICE_TYPE_NULL;

	return device->backend->type;
}


device_status_t
device_set_cancel (device_t *device, device_cancel_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	device->cancel_callback = callback;
	device->cancel_userdata = userdata;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
device_set_events (device_t *device, unsigned int events, device_event_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	device->event_mask = events;
	device->event_callback = callback;
	device->event_userdata = userdata;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
device_set_fingerprint (device_t *device, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->set_fingerprint == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->set_fingerprint (device, data, size);
}


device_status_t
device_version (device_t *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->version == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->version (device, data, size);
}


device_status_t
device_read (device_t *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->read == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->read (device, address, data, size);
}


device_status_t
device_write (device_t *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->write == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->write (device, address, data, size);
}


device_status_t
device_dump (device_t *device, dc_buffer_t *buffer)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->dump == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->dump (device, buffer);
}


device_status_t
device_dump_read (device_t *device, unsigned char data[], unsigned int size, unsigned int blocksize)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->read == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = size;
	device_event_emit (device, DEVICE_EVENT_PROGRESS, &progress);

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > blocksize)
			len = blocksize;

		// Read the packet.
		device_status_t rc = device->backend->read (device, nbytes, data + nbytes, len);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (device, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
device_foreach (device_t *device, dive_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->foreach == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->foreach (device, callback, userdata);
}


device_status_t
device_close (device_t *device)
{
	if (device == NULL)
		return DEVICE_STATUS_SUCCESS;

	if (device->backend->close == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->close (device);
}


void
device_event_emit (device_t *device, device_event_t event, const void *data)
{
	device_progress_t *progress = (device_progress_t *) data;

	// Check the event data for errors.
	switch (event) {
	case DEVICE_EVENT_WAITING:
		assert (data == NULL);
		break;
	case DEVICE_EVENT_PROGRESS:
		assert (progress != NULL);
		assert (progress->maximum != 0);
		assert (progress->maximum >= progress->current);
		break;
	case DEVICE_EVENT_DEVINFO:
		assert (data != NULL);
		break;
	case DEVICE_EVENT_CLOCK:
		assert (data != NULL);
		break;
	default:
		break;
	}

	// Check if there is a callback function registered.
	if (device == NULL || device->event_callback == NULL)
		return;

	// Check the event mask.
	if ((event & device->event_mask) == 0)
		return;

	device->event_callback (device, event, data, device->event_userdata);
}


int
device_is_cancelled (device_t *device)
{
	if (device == NULL)
		return 0;

	if (device->cancel_callback == NULL)
		return 0;

	return device->cancel_callback (device->cancel_userdata);
}
