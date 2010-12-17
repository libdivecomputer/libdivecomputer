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

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include "device-private.h"
#include "uwatec_smart.h"
#include "irda.h"
#include "array.h"
#include "utils.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct uwatec_smart_device_t {
	device_t base;
	irda_t *socket;
	unsigned int address;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_smart_device_t;

static device_status_t uwatec_smart_device_set_fingerprint (device_t *device, const unsigned char data[], unsigned int size);
static device_status_t uwatec_smart_device_version (device_t *abstract, unsigned char data[], unsigned int size);
static device_status_t uwatec_smart_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t uwatec_smart_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t uwatec_smart_device_close (device_t *abstract);

static const device_backend_t uwatec_smart_device_backend = {
	DEVICE_TYPE_UWATEC_SMART,
	uwatec_smart_device_set_fingerprint, /* set_fingerprint */
	uwatec_smart_device_version, /* version */
	NULL, /* read */
	NULL, /* write */
	uwatec_smart_device_dump, /* dump */
	uwatec_smart_device_foreach, /* foreach */
	uwatec_smart_device_close /* close */
};

static int
device_is_uwatec_smart (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_smart_device_backend;
}


static void
uwatec_smart_discovery (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) userdata;
	if (device == NULL)
		return;

	if (strncmp (name, "UWATEC Galileo Sol", 18) == 0 ||
		strncmp (name, "Uwatec Smart", 12) == 0 ||
		strstr (name, "Uwatec") != NULL ||
		strstr (name, "UWATEC") != NULL ||
		strstr (name, "Aladin") != NULL ||
		strstr (name, "ALADIN") != NULL ||
		strstr (name, "Smart") != NULL ||
		strstr (name, "SMART") != NULL ||
		strstr (name, "Galileo") != NULL ||
		strstr (name, "GALILEO") != NULL)
	{
		device->address = address;
	}
}


static device_status_t
uwatec_smart_transfer (uwatec_smart_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	int n = irda_socket_write (device->socket, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	n = irda_socket_read (device->socket, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_handshake (uwatec_smart_device_t *device)
{
	// Command template.
	unsigned char answer[1] = {0};
	unsigned char command[5] = {0x00, 0x10, 0x27, 0, 0};

	// Handshake (stage 1).
	command[0] = 0x1B;
	device_status_t rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	command[0] = 0x1C;
	rc = uwatec_smart_transfer (device, command, 5, answer, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_device_open (device_t **out)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	uwatec_smart_device_t *device = (uwatec_smart_device_t *) malloc (sizeof (uwatec_smart_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &uwatec_smart_device_backend);

	// Set the default values.
	device->socket = NULL;
	device->address = 0;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	irda_init ();

	// Open the irda socket.
	int rc = irda_socket_open (&device->socket);
	if (rc == -1) {
		WARNING ("Failed to open the irda socket.");
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Discover the device.
	rc = irda_socket_discover (device->socket, uwatec_smart_discovery, device);
	if (rc == -1) {
		WARNING ("Failed to discover the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	if (device->address == 0) {
		WARNING ("No dive computer found.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_ERROR;
	}

	// Connect the device.
	rc = irda_socket_connect_lsap (device->socket, device->address, 1);
	if (rc == -1) {
		WARNING ("Failed to connect the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Perform the handshaking.
	uwatec_smart_handshake (device);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_close (device_t *abstract)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (irda_socket_close (device->socket) == -1) {
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	irda_cleanup ();

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != 4)
		return DEVICE_STATUS_ERROR;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t *) abstract;

	if (size < UWATEC_SMART_VERSION_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	unsigned char command[1] = {0};

	// Model Number.
	command[0] = 0x10;
	device_status_t rc = uwatec_smart_transfer (device, command, 1, data + 0, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Serial Number.
	command[0] = 0x14;
	rc = uwatec_smart_transfer (device, command, 1, data + 1, 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Current Timestamp.
	command[0] = 0x1A;
	rc = uwatec_smart_transfer (device, command, 1, data + 5, 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

	// Read the version and clock data.
	unsigned char version[UWATEC_SMART_VERSION_SIZE] = {0};
	device_status_t rc = uwatec_smart_device_version (abstract, version, sizeof (version));
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (version + 5);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	device_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DEVICE_EVENT_CLOCK, &clock);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = version[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (version + 1);
	device_event_emit (&device->base, DEVICE_EVENT_DEVINFO, &devinfo);

	// Command template.
	unsigned char answer[4] = {0};
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	command[0] = 0xC6;
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

  	if (length == 0)
  		return DEVICE_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		WARNING ("Received an unexpected size.");
		return DEVICE_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0;
	while (nbytes < length) {
		// Set the minimum packet size.
		unsigned int len = 32;

		// Increase the packet size if more data is immediately available.
		int available = irda_socket_available (device->socket);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > length)
			len = length - nbytes;

		int n = irda_socket_read (device->socket, data + nbytes, len);
		if (n != len) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += n;
		device_event_emit (&device->base, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += n;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = uwatec_smart_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_smart_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
uwatec_smart_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	if (abstract && !device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DEVICE_STATUS_ERROR;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DEVICE_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}
