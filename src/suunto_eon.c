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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "suunto_eon.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define FP_OFFSET 6
#define FP_SIZE   5

typedef struct suunto_eon_device_t suunto_eon_device_t;

struct suunto_eon_device_t {
	device_t base;
	struct serial *port;
	unsigned char fingerprint[FP_SIZE];
};

static device_status_t suunto_eon_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t suunto_eon_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t suunto_eon_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t suunto_eon_device_close (device_t *abstract);

static const device_backend_t suunto_eon_device_backend = {
	DEVICE_TYPE_SUUNTO_EON,
	suunto_eon_device_set_fingerprint, /* set_fingerprint */
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	suunto_eon_device_dump, /* dump */
	suunto_eon_device_foreach, /* foreach */
	suunto_eon_device_close /* close */
};

static int
device_is_suunto_eon (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_eon_device_backend;
}


device_status_t
suunto_eon_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_eon_device_t *device = (suunto_eon_device_t *) malloc (sizeof (suunto_eon_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_eon_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, FP_SIZE);

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (1200 8N2).
	rc = serial_configure (device->port, 1200, 8, SERIAL_PARITY_NONE, 2, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, -1) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Clear the RTS line.
	if (serial_set_rts (device->port, 0)) {
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_eon_device_close (device_t *abstract)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
suunto_eon_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != FP_SIZE)
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, FP_SIZE);
	else
		memset (device->fingerprint, 0, FP_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_eon_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_EON_MEMORY_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SUUNTO_EON_MEMORY_SIZE + 1;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'P'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	// Receive the answer.
	unsigned char answer[SUUNTO_EON_MEMORY_SIZE + 1] = {0};
	rc = serial_read (device->port, answer, sizeof (answer));
	if (rc != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	// Update and emit a progress event.
	progress.current += sizeof (answer);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = checksum_add_uint8 (answer, sizeof (answer) - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	memcpy (data, answer, SUUNTO_EON_MEMORY_SIZE);

	if (result)
		*result = SUUNTO_EON_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_eon_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	unsigned char data[SUUNTO_EON_MEMORY_SIZE] = {0};

	device_status_t rc = suunto_eon_device_dump (abstract, data, sizeof (data), NULL);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint24_be (data + 244);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	return suunto_eon_extract_dives (abstract, data, sizeof (data), callback, userdata);
}


device_status_t
suunto_eon_device_write_name (device_t *abstract, unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size > 20)
		return DEVICE_STATUS_ERROR;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_eon_device_write_interval (device_t *abstract, unsigned char interval)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (! device_is_suunto_eon (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


static int
fp_compare (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	return memcmp (data + FP_OFFSET, device->fingerprint, FP_SIZE);
}


device_status_t
suunto_eon_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= SUUNTO_EON_MEMORY_SIZE);

	// Search the end-of-profile marker.
	unsigned int eop = 0x100;
	while (eop < SUUNTO_EON_MEMORY_SIZE) {
		if (data[eop] == 0x82) {
			break;
		}
		eop++;
	}

	return suunto_common_extract_dives (abstract, data, 0x100, SUUNTO_EON_MEMORY_SIZE, eop, 3, fp_compare, callback, userdata);
}
