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

#include "device-private.h"
#include "oceanic_common.h"
#include "oceanic_atom2.h"
#include "serial.h"
#include "utils.h"
#include "array.h"
#include "ringbuffer.h"
#include "checksum.h"

#define MAXRETRIES 2

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_atom2_device_t {
	oceanic_common_device_t base;
	serial_t *port;
	unsigned char version[PAGESIZE];
} oceanic_atom2_device_t;

static device_status_t oceanic_atom2_device_version (device_t *abstract, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static device_status_t oceanic_atom2_device_close (device_t *abstract);

static const device_backend_t oceanic_atom2_device_backend = {
	DEVICE_TYPE_OCEANIC_ATOM2,
	oceanic_common_device_set_fingerprint, /* set_fingerprint */
	oceanic_atom2_device_version, /* version */
	oceanic_atom2_device_read, /* read */
	oceanic_atom2_device_write, /* write */
	oceanic_common_device_dump, /* dump */
	oceanic_common_device_foreach, /* foreach */
	oceanic_atom2_device_close /* close */
};

static const unsigned char aeris_atmosai_version[] = "ATMOSAI R\0\0 512K";
static const unsigned char aeris_epic_version[]  = "2M EPIC r\0\0 512K";
static const unsigned char oceanic_proplus2_version[] = "PROPLUS2 \0\0 512K";
static const unsigned char oceanic_atom1_version[] = "ATOM rev\0\0  256K";
static const unsigned char oceanic_atom2_version[] = "2M ATOM r\0\0 512K";
static const unsigned char oceanic_atom3_version[] = "OCEATOM3 \0\0 1024";
static const unsigned char oceanic_vt4_version[]   = "OCEANVT4 \0\0 1024";
static const unsigned char oceanic_geo2_version[]  = "OCEGEO20 \0\0 512K";
static const unsigned char oceanic_oc1_version[]   = "OCWATCH R\0\0 1024";
static const unsigned char oceanic_veo2_version[]  = "OCEVEO20 \0\0 512K";
static const unsigned char oceanic_veo3_version[]  = "OCEVEO30 \0\0 512K";
static const unsigned char sherwood_insight_version[] = "INSIGHT2 \0\0 512K";
static const unsigned char sherwood_wisdom2_version[] = "WISDOM R\0\0  512K";
static const unsigned char tusa_element2_version[] = "ELEMENT2 \0\0 512K";
static const unsigned char tusa_zen_version[]      = "TUSAZEN \0\0  512K";
static const unsigned char tusa_zenair_version[]   = "TUZENAIR \0\0 512K";

static const oceanic_common_layout_t oceanic_default_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_atom1_layout = {
	0x8000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_atom2a_layout = {
	0xFFF0, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_atom2b_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_atom2c_layout = {
	0xFFF0, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t tusa_zenair_layout = {
	0xFFF0, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_oc1_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};


static int
device_is_oceanic_atom2 (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &oceanic_atom2_device_backend;
}


static device_status_t
oceanic_atom2_send (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char ack)
{
	device_t *abstract = (device_t *) device;

	if (device_is_cancelled (abstract))
		return DEVICE_STATUS_CANCELLED;

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = 0;
	n = serial_read (device->port, &response, 1);
	if (n != 1) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the response of the dive computer.
	if (response != ack) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = oceanic_atom2_send (device, command, csize, ACK)) != DEVICE_STATUS_SUCCESS) {
		if (rc != DEVICE_STATUS_TIMEOUT && rc != DEVICE_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Delay the next attempt.
		serial_sleep (100);
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
	}

	if (asize) {
		// Receive the answer of the dive computer.
		int n = serial_read (device->port, answer, asize);
		if (n != asize) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Verify the checksum of the answer.
		unsigned char crc = answer[asize - 1];
		unsigned char ccrc = checksum_add_uint8 (answer, asize - 1, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_quit (oceanic_atom2_device_t *device)
{
	// Send the command to the dive computer.
	unsigned char command[4] = {0x6A, 0x05, 0xA5, 0x00};
	device_status_t rc = oceanic_atom2_send (device, command, sizeof (command), NAK);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
oceanic_atom2_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t *) malloc (sizeof (oceanic_atom2_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base, &oceanic_atom2_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->version, 0, sizeof (device->version));

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000 ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (automatically activated
	// by connecting the device), or already in download mode.
	device_status_t status = oceanic_atom2_device_version ((device_t *) device, device->version, sizeof (device->version));
	if (status != DEVICE_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Override the base class values.
	if (oceanic_common_match (oceanic_oc1_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_atom3_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_vt4_version, device->version, sizeof (device->version)))
		device->base.layout = &oceanic_oc1_layout;
	else if (oceanic_common_match (tusa_zenair_version, device->version, sizeof (device->version)))
		device->base.layout = &tusa_zenair_layout;
	else if (oceanic_common_match (oceanic_atom1_version, device->version, sizeof (device->version)))
		device->base.layout = &oceanic_atom1_layout;
	else if (oceanic_common_match (sherwood_insight_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (sherwood_wisdom2_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (aeris_atmosai_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_geo2_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_proplus2_version, device->version, sizeof (device->version)) ||
		(oceanic_common_match (oceanic_atom2_version, device->version, sizeof (device->version)) &&
		 array_uint16_be (device->version + 0x09) >= 0x3349))
		device->base.layout = &oceanic_atom2a_layout;
	else if (oceanic_common_match (oceanic_veo2_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_veo3_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (tusa_element2_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (tusa_zen_version, device->version, sizeof (device->version)))
		device->base.layout = &oceanic_atom2b_layout;
	else if (oceanic_common_match (aeris_epic_version, device->version, sizeof (device->version)) ||
		oceanic_common_match (oceanic_atom2_version, device->version, sizeof (device->version)))
		device->base.layout = &oceanic_atom2c_layout;
	else
		device->base.layout = &oceanic_default_layout;

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_close (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the quit command.
	oceanic_atom2_quit (device);

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
oceanic_atom2_device_keepalive (device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[4] = {0x91, 0x05, 0xA5, 0x00};
	device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < PAGESIZE)
		return DEVICE_STATUS_MEMORY;

	unsigned char answer[PAGESIZE + 1] = {0};
	unsigned char command[2] = {0x84, 0x00};
	device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer, PAGESIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DEVICE_STATUS_ERROR;
	
	// The data transmission is split in packages
	// of maximum $PAGESIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / PAGESIZE;
		unsigned char answer[PAGESIZE + 1] = {0};
		unsigned char command[4] = {0xB1, 
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0};
		device_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer, PAGESIZE);

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
oceanic_atom2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (! device_is_oceanic_atom2 (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DEVICE_STATUS_ERROR;

	// The data transmission is split in packages
	// of maximum $PAGESIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Prepare to write the package.
		unsigned int number = address / PAGESIZE;
		unsigned char prepare[4] = {0xB2,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0x00};
		device_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), NULL, 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char command[PAGESIZE + 2] = {0};
		memcpy (command, data, PAGESIZE);
		command[PAGESIZE] = checksum_add_uint8 (command, PAGESIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DEVICE_STATUS_SUCCESS;
}
