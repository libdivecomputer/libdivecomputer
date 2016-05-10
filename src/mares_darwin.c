/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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
#include <assert.h>

#include <libdivecomputer/mares_darwin.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "device-private.h"
#include "mares_common.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_darwin_device_vtable)

#define DARWIN    0
#define DARWINAIR 1

typedef struct mares_darwin_layout_t {
	// Memory size.
	unsigned int memsize;
	// Logbook ringbuffer.
	unsigned int rb_logbook_offset;
	unsigned int rb_logbook_size;
	unsigned int rb_logbook_count;
	// Profile ringbuffer
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	// Sample size
	unsigned int samplesize;
} mares_darwin_layout_t;

typedef struct mares_darwin_device_t {
	mares_common_device_t base;
	const mares_darwin_layout_t *layout;
	unsigned int model;
	unsigned char fingerprint[6];
} mares_darwin_device_t;

static dc_status_t mares_darwin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_darwin_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_darwin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mares_darwin_device_close (dc_device_t *abstract);

static const dc_device_vtable_t mares_darwin_device_vtable = {
	sizeof(mares_darwin_device_t),
	DC_FAMILY_MARES_DARWIN,
	mares_darwin_device_set_fingerprint, /* set_fingerprint */
	mares_common_device_read, /* read */
	NULL, /* write */
	mares_darwin_device_dump, /* dump */
	mares_darwin_device_foreach, /* foreach */
	mares_darwin_device_close /* close */
};

static const mares_darwin_layout_t mares_darwin_layout = {
	0x4000, /* memsize */
	0x0100, /* rb_logbook_offset */
	52,     /* rb_logbook_size */
	50,     /* rb_logbook_count */
	0x0B30, /* rb_profile_begin */
	0x4000, /* rb_profile_end */
	2       /* samplesize */
};

static const mares_darwin_layout_t mares_darwinair_layout = {
	0x4000, /* memsize */
	0x0100, /* rb_logbook_offset */
	60,     /* rb_logbook_size */
	50,     /* rb_logbook_count */
	0x0CC0, /* rb_profile_begin */
	0x3FFF, /* rb_profile_end */
	3       /* samplesize */
};


dc_status_t
mares_darwin_device_open (dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_darwin_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mares_darwin_device_t *) dc_device_allocate (context, &mares_darwin_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	mares_common_device_init (&device->base);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->model = model;
	if (model == DARWINAIR)
		device->layout = &mares_darwinair_layout;
	else
		device->layout = &mares_darwin_layout;

	// Open the device.
	status = dc_serial_open (&device->base.port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->base.port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_serial_set_timeout (device->base.port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Set the DTR line.
	status = dc_serial_set_dtr (device->base.port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_close;
	}

	// Set the RTS line.
	status = dc_serial_set_rts (device->base.port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_sleep (device->base.port, 100);
	dc_serial_purge (device->base.port, DC_DIRECTION_ALL);

	// Override the base class values.
	device->base.echo = 1;
	device->base.delay = 50;

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->base.port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
mares_darwin_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_darwin_device_t *device = (mares_darwin_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->base.port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
mares_darwin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_darwin_device_t *device = (mares_darwin_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_darwin_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	mares_darwin_device_t *device = (mares_darwin_device_t *) abstract;

	assert (device->layout != NULL);

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), PACKETSIZE);
}


static dc_status_t
mares_darwin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	mares_darwin_device_t *device = (mares_darwin_device_t *) abstract;

	assert (device->layout != NULL);

	dc_buffer_t *buffer = dc_buffer_new (device->layout->memsize);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = mares_darwin_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (data + 8);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = mares_darwin_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
mares_darwin_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	mares_darwin_device_t *device = (mares_darwin_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	assert (device->layout != NULL);

	const mares_darwin_layout_t *layout = device->layout;

	// Get the profile pointer.
	unsigned int eop = array_uint16_be (data + 0x8A);
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%04x).", eop);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the logbook index.
	unsigned int last = data[0x8C];
	if (last >= layout->rb_logbook_count) {
		ERROR (abstract->context, "Invalid ringbuffer pointer detected (0x%02x).", last);
		return DC_STATUS_DATAFORMAT;
	}

	// Allocate memory for the largest possible dive.
	unsigned char *buffer = (unsigned char *) malloc (layout->rb_logbook_size + layout->rb_profile_end - layout->rb_profile_begin);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// The logbook ringbuffer can store a fixed amount of entries, but there
	// is no guarantee that the profile ringbuffer will contain a profile for
	// each entry. The number of remaining bytes (which is initialized to the
	// largest possible value) is used to detect the last valid profile.
	unsigned int remaining = layout->rb_profile_end - layout->rb_profile_begin;

	unsigned int current = eop;
	for (unsigned int i = 0; i < layout->rb_logbook_count; ++i) {
		// Get the offset to the current logbook entry in the ringbuffer.
		unsigned int idx = (layout->rb_logbook_count + last - i) % layout->rb_logbook_count;
		unsigned int offset = layout->rb_logbook_offset + idx * layout->rb_logbook_size;

		// Get the length of the current dive.
		unsigned int nsamples = array_uint16_be (data + offset + 6);
		unsigned int length = nsamples * layout->samplesize;
		if (nsamples == 0xFFFF || length > remaining)
			break;

		// Copy the logbook entry.
		memcpy (buffer, data + offset, layout->rb_logbook_size);

		// Copy the profile data.
		if (current < layout->rb_profile_begin + length) {
			unsigned int a = current - layout->rb_profile_begin;
			unsigned int b = length - a;
			memcpy (buffer + layout->rb_logbook_size, data + layout->rb_profile_end - b, b);
			memcpy (buffer + layout->rb_logbook_size + b, data + layout->rb_profile_begin, a);
			current = layout->rb_profile_end - b;
		} else {
			memcpy (buffer + layout->rb_logbook_size, data + current - length, length);
			current -= length;
		}

		if (device && memcmp (buffer, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (buffer, layout->rb_logbook_size + length, buffer, 6, userdata)) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}

		remaining -= length;
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}
