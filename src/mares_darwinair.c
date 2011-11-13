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

#include "device-private.h"
#include "mares_common.h"
#include "mares_darwinair.h"
#include "units.h"
#include "utils.h"
#include "array.h"

#define MEMORYSIZE        0x4000

#define RB_LOGBOOK_OFFSET 0x0100
#define RB_LOGBOOK_SIZE   60
#define RB_LOGBOOK_COUNT  50

#define RB_PROFILE_BEGIN  0x0CC0
#define RB_PROFILE_END    0x3FFF

typedef struct mares_darwinair_device_t {
	mares_common_device_t base;
	unsigned char fingerprint[6];
} mares_darwinair_device_t;

static device_status_t mares_darwinair_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t mares_darwinair_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t mares_darwinair_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t mares_darwinair_device_close (device_t *abstract);

static const device_backend_t mares_darwinair_device_backend = {
	DEVICE_TYPE_MARES_DARWINAIR,
	mares_darwinair_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	mares_common_device_read, /* read */
	NULL, /* write */
	mares_darwinair_device_dump, /* dump */
	mares_darwinair_device_foreach, /* foreach */
	mares_darwinair_device_close /* close */
};

static int
device_is_mares_darwinair (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_darwinair_device_backend;
}

device_status_t
mares_darwinair_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_darwinair_device_t *device = (mares_darwinair_device_t *) malloc (sizeof (mares_darwinair_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	mares_common_device_init (&device->base, &mares_darwinair_device_backend);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->base.port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->base.port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->base.port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->base.port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->base.port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->base.port, 1) == -1 ||
		serial_set_rts (device->base.port, 1) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->base.port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->base.port, SERIAL_QUEUE_BOTH);

	// Override the base class values.
	device->base.echo = 1;

	*out = (device_t *) device;

	return DEVICE_STATUS_SUCCESS;
}

static device_status_t
mares_darwinair_device_close (device_t *abstract)
{
	mares_darwinair_device_t *device = (mares_darwinair_device_t *) abstract;

	// Close the device.
	if (serial_close (device->base.port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_darwinair_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_darwinair_device_t *device = (mares_darwinair_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_darwinair_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, MEMORYSIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), PACKETSIZE);
}


static device_status_t
mares_darwinair_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (MEMORYSIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = mares_darwinair_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (data + 8);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = mares_darwinair_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
mares_darwinair_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	mares_darwinair_device_t *device = (mares_darwinair_device_t *) abstract;

	if (abstract && !device_is_mares_darwinair (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Get the profile pointer.
	unsigned int eop = array_uint16_be (data + 0x8A);
	if (eop < RB_PROFILE_BEGIN || eop >= RB_PROFILE_END) {
		WARNING ("Invalid ringbuffer pointer detected.");
		return DEVICE_STATUS_ERROR;
	}

	// Get the logbook index.
	unsigned int last = data[0x8C];
	if (last >= RB_LOGBOOK_COUNT) {
		WARNING ("Invalid ringbuffer pointer detected.");
		return DEVICE_STATUS_ERROR;
	}

	// Allocate memory for the largest possible dive.
	unsigned char *buffer = malloc (RB_LOGBOOK_SIZE + RB_PROFILE_END - RB_PROFILE_BEGIN);
	if (buffer == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// The logbook ringbuffer can store a fixed amount of entries, but there
	// is no guarantee that the profile ringbuffer will contain a profile for
	// each entry. The number of remaining bytes (which is initialized to the
	// largest possible value) is used to detect the last valid profile.
	unsigned int remaining = RB_PROFILE_END - RB_PROFILE_BEGIN;

	unsigned int current = eop;
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		// Get the offset to the current logbook entry in the ringbuffer.
		unsigned int idx = (RB_LOGBOOK_COUNT + last - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = RB_LOGBOOK_OFFSET + idx * RB_LOGBOOK_SIZE;

		// Get the length of the current dive.
		unsigned int nsamples = array_uint16_be (data + offset + 6);
		unsigned int length = nsamples * 3;
		if (nsamples == 0xFFFF || length > remaining)
			break;

		// Copy the logbook entry.
		memcpy (buffer, data + offset, RB_LOGBOOK_SIZE);

		// Copy the profile data.
		if (current < RB_PROFILE_BEGIN + length) {
			unsigned int a = current - RB_PROFILE_BEGIN;
			unsigned int b = length - a;
			memcpy (buffer + RB_LOGBOOK_SIZE, data + RB_PROFILE_END - b, b);
			memcpy (buffer + RB_LOGBOOK_SIZE + b, data + RB_PROFILE_BEGIN, a);
			current = RB_PROFILE_END - b;
		} else {
			memcpy (buffer + RB_LOGBOOK_SIZE, data + current - length, length);
			current -= length;
		}

		if (device && memcmp (buffer, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (callback && !callback (buffer, RB_LOGBOOK_SIZE + length, buffer, 6, userdata)) {
			free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		remaining -= length;
	}

	free (buffer);

	return DEVICE_STATUS_SUCCESS;
}
