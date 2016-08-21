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

#include <string.h> // memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>

#include <libdivecomputer/oceanic_vtpro.h>

#include "context-private.h"
#include "device-private.h"
#include "oceanic_common.h"
#include "serial.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_vtpro_device_vtable.base)

#define MAXRETRIES 2
#define MULTIPAGE  4

#define ACK 0x5A
#define NAK 0xA5
#define END 0x51

#define AERIS500AI 0x4151

typedef enum oceanic_vtpro_protocol_t {
	MOD,
	INTR,
} oceanic_vtpro_protocol_t;

typedef struct oceanic_vtpro_device_t {
	oceanic_common_device_t base;
	dc_serial_t *port;
	unsigned int model;
	oceanic_vtpro_protocol_t protocol;
} oceanic_vtpro_device_t;

static dc_status_t oceanic_vtpro_device_logbook (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook);
static dc_status_t oceanic_vtpro_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_vtpro_device_close (dc_device_t *abstract);

static const oceanic_common_device_vtable_t oceanic_vtpro_device_vtable = {
	{
		sizeof(oceanic_vtpro_device_t),
		DC_FAMILY_OCEANIC_VTPRO,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		oceanic_vtpro_device_read, /* read */
		NULL, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		oceanic_vtpro_device_close /* close */
	},
	oceanic_vtpro_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_version_t oceanic_vtpro_version[] = {
	{"VERSAPRO \0\0 256K"},
	{"ATMOSTWO \0\0 256K"},
	{"PROPLUS2 \0\0 256K"},
	{"ATMOSAIR \0\0 256K"},
	{"VTPRO  r\0\0  256K"},
	{"ELITE  r\0\0  256K"},
};

static const oceanic_common_version_t oceanic_wisdom_version[] = {
	{"WISDOM r\0\0  256K"},
};

static const oceanic_common_layout_t oceanic_vtpro_layout = {
	0x8000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0440, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0440, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_wisdom_layout = {
	0x8000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03D0, /* rb_logbook_begin */
	0x05D0, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x05D0, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_500ai_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0110, /* cf_pointers */
	0x0200, /* rb_logbook_begin */
	0x0200, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x00200, /* rb_profile_begin */
	0x20000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	2, /* pt_mode_serial */
};

static dc_status_t
oceanic_vtpro_send (oceanic_vtpro_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command to the dive computer.
	status = dc_serial_write (device->port, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = NAK;
	status = dc_serial_read (device->port, &response, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the response of the dive computer.
	if (response != ACK) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_transfer (oceanic_vtpro_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_vtpro_send (device, command, csize)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;
	}

	if (asize) {
		// Receive the answer of the dive computer.
		status = dc_serial_read (device->port, answer, asize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_init (oceanic_vtpro_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2][2] = {
		{0xAA, 0x00},
		{0x20, 0x00}};
	status = dc_serial_write (device->port, command[device->protocol], sizeof (command[device->protocol]), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer of the dive computer.
	unsigned char answer[13] = {0};
	status = dc_serial_read (device->port, answer, sizeof (answer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the answer.
	const unsigned char response[2][13] = {
		{0x4D, 0x4F, 0x44, 0x2D, 0x2D, 0x4F, 0x4B, 0x5F, 0x56, 0x32, 0x2E, 0x30, 0x30},
		{0x49, 0x4E, 0x54, 0x52, 0x2D, 0x4F, 0x4B, 0x5F, 0x56, 0x31, 0x2E, 0x31, 0x31}};
	if (memcmp (answer, response[device->protocol], sizeof (response[device->protocol])) != 0) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_quit (oceanic_vtpro_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char answer[1] = {0};
	unsigned char command[4] = {0x6A, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the last byte of the answer.
	if (answer[0] != END) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_calibrate (oceanic_vtpro_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	// The timeout is temporary increased, because the
	// device needs approximately 6 seconds to respond.
	unsigned char answer[2] = {0};
	unsigned char command[2] = {0x18, 0x00};
	dc_serial_set_timeout (device->port, 9000);
	dc_status_t rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, sizeof (answer));
	dc_serial_set_timeout (device->port, 3000);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the last byte of the answer.
	if (answer[1] != 0x00) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceanic_aeris500ai_device_logbook (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t *) abstract;

	assert (device != NULL);
	assert (device->base.layout != NULL);
	assert (device->base.layout->rb_logbook_entry_size == PAGESIZE / 2);
	assert (device->base.layout->rb_logbook_begin == device->base.layout->rb_logbook_end);
	assert (progress != NULL);

	const oceanic_common_layout_t *layout = device->base.layout;

	// Erase the buffer.
	if (!dc_buffer_clear (logbook))
		return DC_STATUS_NOMEMORY;

	// Read the pointer data.
	unsigned char pointers[PAGESIZE] = {0};
	rc = oceanic_vtpro_device_read (abstract, layout->cf_pointers, pointers, sizeof (pointers));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory page.");
		return rc;
	}

	// Get the logbook pointers.
	unsigned int last = pointers[0x03];

	// Update and emit a progress event.
	progress->current += PAGESIZE;
	progress->maximum += PAGESIZE + (last + 1) * PAGESIZE / 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

	// Allocate memory for the logbook entries.
	if (!dc_buffer_reserve (logbook, (last + 1) * PAGESIZE / 2))
		return DC_STATUS_NOMEMORY;

	// Send the logbook index command.
	unsigned char command[] = {0x52,
			(last >> 8) & 0xFF, // high
			(last     ) & 0xFF, // low
			0x00};
	rc = oceanic_vtpro_transfer (device, command, sizeof (command), NULL, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the logbook index command.");
		return rc;
	}

	// Read the logbook index.
	for (unsigned int i = 0; i < last + 1; ++i) {
		// Receive the answer of the dive computer.
		unsigned char answer[PAGESIZE / 2 + 1] = {0};
		rc = dc_serial_read (device->port, answer, sizeof(answer), NULL);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return rc;
		}

		// Verify the checksum of the answer.
		unsigned char crc = answer[PAGESIZE / 2];
		unsigned char ccrc = checksum_add_uint4 (answer, PAGESIZE / 2, 0x00);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		progress->current += PAGESIZE / 2;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);

		// Ignore uninitialized entries.
		if (array_isequal (answer, PAGESIZE / 2, 0xFF)) {
			WARNING (abstract->context, "Uninitialized logbook entries detected!");
			continue;
		}

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (answer, device->base.fingerprint, PAGESIZE / 2) == 0) {
			dc_buffer_clear (logbook);
		} else {
			dc_buffer_append (logbook, answer, PAGESIZE / 2);
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceanic_vtpro_device_logbook (dc_device_t *abstract, dc_event_progress_t *progress, dc_buffer_t *logbook)
{
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t *) abstract;

	if (device->model == AERIS500AI) {
		return oceanic_aeris500ai_device_logbook (abstract, progress, logbook);
	} else {
		return oceanic_common_device_logbook (abstract, progress, logbook);
	}
}

dc_status_t
oceanic_vtpro_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	return oceanic_vtpro_device_open2 (out, context, name, 0);
}


dc_status_t
oceanic_vtpro_device_open2 (dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_vtpro_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceanic_vtpro_device_t *) dc_device_allocate (context, &oceanic_vtpro_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Override the base class values.
	device->base.multipage = MULTIPAGE;

	// Set the default values.
	device->port = NULL;
	device->model = model;
	if (model == AERIS500AI) {
		device->protocol = INTR;
	} else {
		device->protocol = MOD;
	}

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_serial_set_timeout (device->port, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Set the DTR line.
	status = dc_serial_set_dtr (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_close;
	}

	// Set the RTS line.
	status = dc_serial_set_rts (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_close;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_serial_sleep (device->port, device->protocol == MOD ? 100 : 1000);

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Initialize the data cable (MOD mode).
	status = oceanic_vtpro_init (device);
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (manually activated by
	// the user), or already in download mode.
	status = oceanic_vtpro_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Calibrate the device. Although calibration is optional, it's highly
	// recommended because it reduces the transfer time considerably, even
	// when processing the command itself is quite slow.
	status = oceanic_vtpro_calibrate (device);
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Override the base class values.
	if (model == AERIS500AI) {
		device->base.layout = &aeris_500ai_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_wisdom_version)) {
		device->base.layout = &oceanic_wisdom_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_vtpro_version)) {
		device->base.layout = &oceanic_vtpro_layout;
	} else {
		WARNING (context, "Unsupported device detected!");
		device->base.layout = &oceanic_vtpro_layout;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
oceanic_vtpro_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Switch the device back to surface mode.
	oceanic_vtpro_quit (device);

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
oceanic_vtpro_device_keepalive (dc_device_t *abstract)
{
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char answer[1] = {0};
	unsigned char command[4] = {0x6A, 0x08, 0x00, 0x00};
	dc_status_t rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the last byte of the answer.
	if (answer[0] != END) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_vtpro_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	// Switch the device into download mode. The response is ignored here,
	// since it is identical (except for the missing trailing byte) to the
	// response of the first part of the other command in this function.

	unsigned char cmd[2] = {0x88, 0x00};
	unsigned char ans[PAGESIZE / 2 + 1] = {0};
	dc_status_t rc = oceanic_vtpro_transfer (device, cmd, sizeof (cmd), ans, sizeof (ans));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the checksum of the answer.
	unsigned char crc = ans[PAGESIZE / 2];
	unsigned char ccrc = checksum_add_uint4 (ans, PAGESIZE / 2, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (device->protocol == MOD) {
		// Obtain the device identification string. This string is
		// split over two packets, but we join both parts again.
		for (unsigned int i = 0; i < 2; ++i) {
			unsigned char command[4] = {0x72, 0x03, i * 0x10, 0x00};
			unsigned char answer[PAGESIZE / 2 + 2] = {0};
			rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, sizeof (answer));
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Verify the checksum of the answer.
			unsigned char crc = answer[PAGESIZE / 2];
			unsigned char ccrc = checksum_add_uint4 (answer, PAGESIZE / 2, 0x00);
			if (crc != ccrc) {
				ERROR (abstract->context, "Unexpected answer checksum.");
				return DC_STATUS_PROTOCOL;
			}

			// Verify the last byte of the answer.
			if (answer[PAGESIZE / 2 + 1] != END) {
				ERROR (abstract->context, "Unexpected answer byte.");
				return DC_STATUS_PROTOCOL;
			}

			// Append the answer to the output buffer.
			memcpy (data + i * PAGESIZE / 2, answer, PAGESIZE / 2);
		}
	} else {
		// Return an empty device identification string.
		memset (data, 0x00, PAGESIZE);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the number of packages.
		unsigned int npackets = (size - nbytes) / PAGESIZE;
		if (npackets > MULTIPAGE)
			npackets = MULTIPAGE;

		// Read the package.
		unsigned int first =  address / PAGESIZE;
		unsigned int last  = first + npackets - 1;
		unsigned char answer[(PAGESIZE + 1) * MULTIPAGE] = {0};
		unsigned char command[6] = {0x34,
				(first >> 8) & 0xFF, // high
				(first     ) & 0xFF, // low
				(last >> 8) & 0xFF, // high
				(last     ) & 0xFF, // low
				0x00};
		dc_status_t rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, (PAGESIZE + 1) * npackets);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		unsigned int offset = 0;
		for (unsigned int i = 0; i < npackets; ++i) {
			// Verify the checksum of the answer.
			unsigned char crc = answer[offset + PAGESIZE];
			unsigned char ccrc = checksum_add_uint8 (answer + offset, PAGESIZE, 0x00);
			if (crc != ccrc) {
				ERROR (abstract->context, "Unexpected answer checksum.");
				return DC_STATUS_PROTOCOL;
			}

			memcpy (data, answer + offset, PAGESIZE);

			offset += PAGESIZE + 1;
			nbytes += PAGESIZE;
			address += PAGESIZE;
			data += PAGESIZE;
		}
	}

	return DC_STATUS_SUCCESS;
}
