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

#include <libdivecomputer/oceanic_veo250.h>

#include "context-private.h"
#include "device-private.h"
#include "oceanic_common.h"
#include "serial.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_veo250_device_vtable)

#define MAXRETRIES 2
#define MULTIPAGE  4

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_veo250_device_t {
	oceanic_common_device_t base;
	serial_t *port;
	unsigned int last;
} oceanic_veo250_device_t;

static dc_status_t oceanic_veo250_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_veo250_device_close (dc_device_t *abstract);

static const dc_device_vtable_t oceanic_veo250_device_vtable = {
	DC_FAMILY_OCEANIC_VEO250,
	oceanic_common_device_set_fingerprint, /* set_fingerprint */
	oceanic_veo250_device_read, /* read */
	NULL, /* write */
	oceanic_common_device_dump, /* dump */
	oceanic_common_device_foreach, /* foreach */
	oceanic_veo250_device_close /* close */
};

static const oceanic_common_version_t oceanic_vtpro_version[] = {
	{"GENREACT \0\0 256K"},
	{"VEO 200 R\0\0 256K"},
	{"VEO 250 R\0\0 256K"},
	{"SEEMANN R\0\0 256K"},
	{"VEO 180 R\0\0 256K"},
	{"AERISXR2 \0\0 256K"},
	{"INSIGHT R\0\0 256K"},
};

static const oceanic_common_layout_t oceanic_veo250_layout = {
	0x8000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0600, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0600, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	1, /* pt_mode_global */
	1 /* pt_mode_logbook */
};


static dc_status_t
oceanic_veo250_send (oceanic_veo250_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Discard garbage bytes.
	serial_flush (device->port, SERIAL_QUEUE_INPUT);

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = NAK;
	n = serial_read (device->port, &response, 1);
	if (n != 1) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the response of the dive computer.
	if (response != ACK) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_transfer (oceanic_veo250_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_veo250_send (device, command, csize)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Delay the next attempt.
		serial_sleep (device->port, 100);
	}

	// Receive the answer of the dive computer.
	int n = serial_read (device->port, answer, asize);
	if (n != asize) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the last byte of the answer.
	if (answer[asize - 1] != NAK) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_init (oceanic_veo250_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2] = {0x55, 0x00};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the answer of the dive computer.
	unsigned char answer[13] = {0};
	n = serial_read (device->port, answer, sizeof (answer));
	if (n != sizeof (answer)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		if (n == 0)
			return DC_STATUS_SUCCESS;
		return EXITCODE (n);
	}

	// Verify the answer.
	const unsigned char response[13] = {
		0x50, 0x50, 0x53, 0x2D, 0x2D, 0x4F, 0x4B,
		0x5F, 0x56, 0x32, 0x2E, 0x30, 0x30};
	if (memcmp (answer, response, sizeof (response)) != 0) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_quit (oceanic_veo250_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2] = {0x98, 0x00};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_veo250_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t *) malloc (sizeof (oceanic_veo250_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base, context, &oceanic_veo250_device_vtable);

	// Override the base class values.
	device->base.layout = &oceanic_veo250_layout;
	device->base.multipage = MULTIPAGE;

	// Set the default values.
	device->port = NULL;
	device->last = 0;

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the DTR and RTS lines.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 1) == -1) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (device->port, 100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Initialize the data cable (PPS mode).
	dc_status_t status = oceanic_veo250_init (device);
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Delay the sending of the version command.
	serial_sleep (device->port, 100);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (manually activated by
	// the user), or already in download mode.
	status = oceanic_veo250_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_device_close (dc_device_t *abstract)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	// Switch the device back to surface mode.
	oceanic_veo250_quit (device);

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_veo250_device_keepalive (dc_device_t *abstract)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[2] = {0};
	unsigned char command[4] = {0x91,
		(device->last     ) & 0xFF, // low
		(device->last >> 8) & 0xFF, // high
		0x00};
	dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != NAK) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_veo250_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[PAGESIZE + 2] = {0};
	unsigned char command[2] = {0x90, 0x00};
	dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the checksum of the answer.
	unsigned char crc = answer[PAGESIZE];
	unsigned char ccrc = checksum_add_uint8 (answer, PAGESIZE, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, answer, PAGESIZE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

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
		unsigned char answer[(PAGESIZE + 1) * MULTIPAGE + 1] = {0};
		unsigned char command[6] = {0x20,
				(first     ) & 0xFF, // low
				(first >> 8) & 0xFF, // high
				(last     ) & 0xFF, // low
				(last >> 8) & 0xFF, // high
				0};
		dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, (PAGESIZE + 1) * npackets + 1);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		device->last = last;

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
