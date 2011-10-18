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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "cressi_edy.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"
#include "ringbuffer.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define PAGESIZE          (CRESSI_EDY_PACKET_SIZE / 4)

#define BASE              0x4000

#define RB_PROFILE_BEGIN  0x4000
#define RB_PROFILE_END    0x7F80

#define RB_LOGBOOK_OFFSET 0x7F80
#define RB_LOGBOOK_BEGIN  0
#define RB_LOGBOOK_END    60

typedef struct cressi_edy_device_t {
	device_t base;
	serial_t *port;
	unsigned char fingerprint[PAGESIZE / 2];
	unsigned int model;
} cressi_edy_device_t;

static device_status_t cressi_edy_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t cressi_edy_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t cressi_edy_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t cressi_edy_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t cressi_edy_device_close (device_t *abstract);

static const device_backend_t cressi_edy_device_backend = {
	DEVICE_TYPE_CRESSI_EDY,
	cressi_edy_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	cressi_edy_device_read, /* read */
	NULL, /* write */
	cressi_edy_device_dump, /* dump */
	cressi_edy_device_foreach, /* foreach */
	cressi_edy_device_close /* close */
};

static int
device_is_cressi_edy (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &cressi_edy_device_backend;
}


static device_status_t
cressi_edy_transfer (cressi_edy_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, int trailer)
{
	assert (asize >= csize);

	// Flush the serial input buffer.
	int rc = serial_flush (device->port, SERIAL_QUEUE_INPUT);
	if (rc == -1) {
		WARNING ("Failed to flush the serial input buffer.");
		return DEVICE_STATUS_IO;
	}

	// Send the command to the device.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the answer of the device.
	n = serial_read (device->port, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the echo.
	if (memcmp (answer, command, csize) != 0) {
		WARNING ("Unexpected echo.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the trailer of the packet.
	if (trailer && answer[asize - 1] != 0x45) {
		WARNING ("Unexpected answer trailer byte.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_init1 (cressi_edy_device_t *device)
{
	unsigned char command[3] = {0x41, 0x42, 0x43};
	unsigned char answer[6] = {0};

	return cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
}


static device_status_t
cressi_edy_init2 (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x44};
	unsigned char answer[2] = {0};

	device_status_t rc = cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	device->model = answer[1];

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_init3 (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x0C};
	unsigned char answer[2] = {0};

	return cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
}


static device_status_t
cressi_edy_quit (cressi_edy_device_t *device)
{
	unsigned char command[1] = {0x46};
	unsigned char answer[1] = {0};

	return cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 0);
}


device_status_t
cressi_edy_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	cressi_edy_device_t *device = (cressi_edy_device_t *) malloc (sizeof (cressi_edy_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &cressi_edy_device_backend);

	// Set the default values.
	device->port = NULL;
	device->model = 0;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (1200 8N1).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR and clear the RTS line.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 0) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Send the init commands.
	cressi_edy_init1 (device);
	cressi_edy_init2 (device);
	cressi_edy_init3 (device);

	// Set the serial communication protocol (4800 8N1).
	rc = serial_configure (device->port, 4800, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_device_close (device_t *abstract)
{
	cressi_edy_device_t *device = (cressi_edy_device_t*) abstract;

	if (! device_is_cressi_edy (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the quit command.
	cressi_edy_quit (device);

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	cressi_edy_device_t *device = (cressi_edy_device_t*) abstract;

	if (! device_is_cressi_edy (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if ((address % (CRESSI_EDY_PACKET_SIZE / 4) != 0) ||
		(size    % CRESSI_EDY_PACKET_SIZE != 0))
		return DEVICE_STATUS_ERROR;

	// The data transmission is split in packages
	// of maximum $CRESSI_EDY_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / (CRESSI_EDY_PACKET_SIZE / 4);
		unsigned char answer[3 + CRESSI_EDY_PACKET_SIZE + 1] = {0};
		unsigned char command[3] = {0x52,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF}; // low
		device_status_t rc = cressi_edy_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 3, CRESSI_EDY_PACKET_SIZE);

		nbytes += CRESSI_EDY_PACKET_SIZE;
		address += CRESSI_EDY_PACKET_SIZE;
		data += CRESSI_EDY_PACKET_SIZE;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	cressi_edy_device_t *device = (cressi_edy_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
cressi_edy_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	if (! device_is_cressi_edy (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, CRESSI_EDY_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), CRESSI_EDY_PACKET_SIZE);
}


static device_status_t
cressi_edy_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	cressi_edy_device_t *device = (cressi_edy_device_t *) abstract;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = CRESSI_EDY_PACKET_SIZE +
		(RB_PROFILE_END - RB_PROFILE_BEGIN);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = 0;
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	// Read the configuration data.
	unsigned char config[CRESSI_EDY_PACKET_SIZE] = {0};
	device_status_t rc = cressi_edy_device_read (abstract, 0x7F80, config, sizeof (config));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to read the configuration data.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += CRESSI_EDY_PACKET_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Get the logbook pointers.
	unsigned int last  = config[0x7C];
	unsigned int first = config[0x7D];
	if (first < RB_LOGBOOK_BEGIN || first >= RB_LOGBOOK_END ||
		last < RB_LOGBOOK_BEGIN || last >= RB_LOGBOOK_END) {
		if (last == 0xFF)
			return DEVICE_STATUS_SUCCESS;
		WARNING ("Invalid ringbuffer pointer detected.");
		return DEVICE_STATUS_ERROR;
	}

	// Get the number of logbook items.
	unsigned int count = ringbuffer_distance (first, last, 0, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END) + 1;

	// Get the profile pointer.
	unsigned int eop = array_uint16_le (config + 0x7E) * PAGESIZE + BASE;
	if (eop < RB_PROFILE_BEGIN || eop >= RB_PROFILE_END) {
		WARNING ("Invalid ringbuffer pointer detected.");
		return DEVICE_STATUS_ERROR;
	}

	// Memory buffer for the profile data.
	unsigned char buffer[RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	unsigned int available = 0;
	unsigned int offset = RB_PROFILE_END - RB_PROFILE_BEGIN;

	unsigned int previous = eop;
	unsigned int address = previous;

	unsigned int idx = last;
	for (unsigned int i = 0; i < count; ++i) {
		// Get the pointer to the profile data.
		unsigned int current = array_uint16_le (config + 2 * idx) * PAGESIZE + BASE;
		if (current < RB_PROFILE_BEGIN || current >= RB_PROFILE_END) {
			WARNING ("Invalid ringbuffer pointer detected.");
			return DEVICE_STATUS_ERROR;
		}

		// Position the pointer at the start of the header.
		if (current == RB_PROFILE_BEGIN)
			current = RB_PROFILE_END;
		current -= PAGESIZE;

		// Get the profile length.
		unsigned int length = ringbuffer_distance (current, previous, 1, RB_PROFILE_BEGIN, RB_PROFILE_END);

		unsigned nbytes = available;
		while (nbytes < length) {
			if (address == RB_PROFILE_BEGIN)
				address = RB_PROFILE_END;
			address -= CRESSI_EDY_PACKET_SIZE;
			offset -= CRESSI_EDY_PACKET_SIZE;

			// Read the memory page.
			rc = cressi_edy_device_read (abstract, address, buffer + offset, CRESSI_EDY_PACKET_SIZE);
			if (rc != DEVICE_STATUS_SUCCESS) {
				WARNING ("Failed to read the memory page.");
				return rc;
			}

			// Update and emit a progress event.
			progress.current += CRESSI_EDY_PACKET_SIZE;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

			nbytes += CRESSI_EDY_PACKET_SIZE;
		}

		available = nbytes - length;
		previous = current;

		unsigned char *p = buffer + offset + available;

		if (memcmp (p, device->fingerprint, sizeof (device->fingerprint)) == 0)
			return DEVICE_STATUS_SUCCESS;

		if (callback && !callback (p, length, p, sizeof (device->fingerprint), userdata))
			return DEVICE_STATUS_SUCCESS;

		if (idx == RB_LOGBOOK_BEGIN)
			idx = RB_LOGBOOK_END;
		idx--;
	}

	return DEVICE_STATUS_SUCCESS;
}
