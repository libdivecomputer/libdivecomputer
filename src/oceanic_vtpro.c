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

#include <libdivecomputer/oceanic_vtpro.h>

#include "context-private.h"
#include "device-private.h"
#include "oceanic_common.h"
#include "serial.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_vtpro_device_vtable)

#define MAXRETRIES 2
#define MULTIPAGE  4

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define ACK 0x5A
#define NAK 0xA5
#define END 0x51

typedef struct oceanic_vtpro_device_t {
	oceanic_common_device_t base;
	serial_t *port;
} oceanic_vtpro_device_t;

static dc_status_t oceanic_vtpro_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_vtpro_device_close (dc_device_t *abstract);

static const dc_device_vtable_t oceanic_vtpro_device_vtable = {
	DC_FAMILY_OCEANIC_VTPRO,
	oceanic_common_device_set_fingerprint, /* set_fingerprint */
	oceanic_vtpro_device_read, /* read */
	NULL, /* write */
	oceanic_common_device_dump, /* dump */
	oceanic_common_device_foreach, /* foreach */
	oceanic_vtpro_device_close /* close */
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
	0 /* pt_mode_logbook */
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
	0 /* pt_mode_logbook */
};


static dc_status_t
oceanic_vtpro_send (oceanic_vtpro_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

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
oceanic_vtpro_transfer (oceanic_vtpro_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
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

	// Receive the answer of the dive computer.
	int n = serial_read (device->port, answer, asize);
	if (n != asize) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_init (oceanic_vtpro_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2] = {0xAA, 0x00};
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
		return EXITCODE (n);
	}

	// Verify the answer.
	const unsigned char response[13] = {
		0x4D, 0x4F, 0x44, 0x2D, 0x2D, 0x4F, 0x4B,
		0x5F, 0x56, 0x32, 0x2E, 0x30, 0x30};
	if (memcmp (answer, response, sizeof (response)) != 0) {
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
	serial_set_timeout (device->port, 9000);
	dc_status_t rc = oceanic_vtpro_transfer (device, command, sizeof (command), answer, sizeof (answer));
	serial_set_timeout (device->port, 3000);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the last byte of the answer.
	if (answer[1] != 0x00) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_vtpro_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t *) malloc (sizeof (oceanic_vtpro_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base, context, &oceanic_vtpro_device_vtable);

	// Override the base class values.
	device->base.multipage = MULTIPAGE;

	// Set the default values.
	device->port = NULL;

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

	// Initialize the data cable (MOD mode).
	dc_status_t status = oceanic_vtpro_init (device);
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (manually activated by
	// the user), or already in download mode.
	status = oceanic_vtpro_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Calibrate the device. Although calibration is optional, it's highly
	// recommended because it reduces the transfer time considerably, even
	// when processing the command itself is quite slow.
	status = oceanic_vtpro_calibrate (device);
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Override the base class values.
	if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_wisdom_version)) {
		device->base.layout = &oceanic_wisdom_layout;
	} else {
		device->base.layout = &oceanic_vtpro_layout;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_device_close (dc_device_t *abstract)
{
	oceanic_vtpro_device_t *device = (oceanic_vtpro_device_t*) abstract;

	// Switch the device back to surface mode.
	oceanic_vtpro_quit (device);

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
