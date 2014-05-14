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

#include <libdivecomputer/oceanic_atom2.h>

#include "context-private.h"
#include "device-private.h"
#include "oceanic_common.h"
#include "serial.h"
#include "array.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_atom2_device_vtable)

#define MAXRETRIES 2
#define MAXDELAY   16

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_atom2_device_t {
	oceanic_common_device_t base;
	serial_t *port;
	unsigned int delay;
} oceanic_atom2_device_t;

static dc_status_t oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_close (dc_device_t *abstract);

static const dc_device_vtable_t oceanic_atom2_device_vtable = {
	DC_FAMILY_OCEANIC_ATOM2,
	oceanic_common_device_set_fingerprint, /* set_fingerprint */
	oceanic_atom2_device_read, /* read */
	oceanic_atom2_device_write, /* write */
	oceanic_common_device_dump, /* dump */
	oceanic_common_device_foreach, /* foreach */
	oceanic_atom2_device_close /* close */
};

static const oceanic_common_version_t aeris_f10_version[] = {
	{"FREEWAER \0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom1_version[] = {
	{"ATOM rev\0\0  256K"},
};

static const oceanic_common_version_t oceanic_atom2_version[] = {
	{"2M ATOM r\0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2a_version[] = {
	{"MANTA  R\0\0  512K"},
	{"WISDOM R\0\0  512K"},
	{"INSIGHT2 \0\0 512K"},
	{"OCEVEO30 \0\0 512K"},
	{"ATMOSAI R\0\0 512K"},
	{"PROPLUS2 \0\0 512K"},
	{"OCEGEO20 \0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2b_version[] = {
	{"ELEMENT2 \0\0 512K"},
	{"OCEVEO20 \0\0 512K"},
	{"TUSAZEN \0\0  512K"},
	{"PROPLUS3 \0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2c_version[] = {
	{"2M EPIC r\0\0 512K"},
	{"EPIC1  R\0\0  512K"},
	{"AERIA300 \0\0 512K"},
};

static const oceanic_common_version_t oceanic_default_version[] = {
	{"OCE VT3 R\0\0 512K"},
	{"ELITET3 R\0\0 512K"},
	{"ELITET31 \0\0 512K"},
	{"DATAMASK \0\0 512K"},
	{"COMPMASK \0\0 512K"},
	{"HOLLDG03 \0\0 512K"},
};

static const oceanic_common_version_t tusa_zenair_version[] = {
	{"TUZENAIR \0\0 512K"},
	{"AMPHOSSW \0\0 512K"},
	{"VOYAGE2G \0\0 512K"},
};

static const oceanic_common_version_t oceanic_oc1_version[] = {
	{"OCWATCH R\0\0 1024"},
	{"OC1WATCH \0\0 1024"},
	{"OCSWATCH \0\0 1024"},
};

static const oceanic_common_version_t oceanic_oci_version[] = {
	{"OCEANOCI \0\0 1024"},
};

static const oceanic_common_version_t oceanic_atom3_version[] = {
	{"OCEATOM3 \0\0 1024"},
	{"ATOM31  \0\0  1024"},
};

static const oceanic_common_version_t oceanic_vt4_version[] = {
	{"OCEANVT4 \0\0 1024"},
	{"OCEAVT41 \0\0 1024"},
	{"AERISAIR \0\0 1024"},
};

static const oceanic_common_version_t hollis_tx1_version[] = {
	{"HOLLDG04 \0\0 2048"},
};

static const oceanic_common_version_t oceanic_veo1_version[] = {
	{"OCEVEO10 \0\0   8K"},
	{"AERIS XR1 NX R\0\0"},
};

static const oceanic_common_version_t oceanic_reactpro_version[] = {
	{"REACPRO2 \0\0 512K"},
};

static const oceanic_common_layout_t aeris_f10_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	0x0D90, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	2 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_default_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
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
	8, /* rb_logbook_entry_size */
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
	8, /* rb_logbook_entry_size */
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
	8, /* rb_logbook_entry_size */
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
	8, /* rb_logbook_entry_size */
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
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_oc1_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_oci_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x1400, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_atom3_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_vt4_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0420, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t hollis_tx1_layout = {
	0x40000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0780, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x1000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
	0, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_veo1_layout = {
	0x0400, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0400, /* rb_profile_begin */
	0x0400, /* rb_profile_end */
	0, /* pt_mode_global */
	0 /* pt_mode_logbook */
};

static const oceanic_common_layout_t oceanic_reactpro_layout = {
	0xFFF0, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0600, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0600, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	1, /* pt_mode_global */
	1 /* pt_mode_logbook */
};

static dc_status_t
oceanic_atom2_send (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char ack)
{
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (device->delay) {
		serial_sleep (device->port, device->delay);
	}

	// Send the command to the dive computer.
	int n = serial_write (device->port, command, csize);
	if (n != csize) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = 0;
	n = serial_read (device->port, &response, 1);
	if (n != 1) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the response of the dive computer.
	if (response != ack) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_atom2_send (device, command, csize, ACK)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Increase the inter packet delay.
		if (device->delay < MAXDELAY)
			device->delay++;

		// Delay the next attempt.
		serial_sleep (device->port, 100);
		serial_flush (device->port, SERIAL_QUEUE_INPUT);
	}

	if (asize) {
		// Receive the answer of the dive computer.
		int n = serial_read (device->port, answer, asize);
		if (n != asize) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Verify the checksum of the answer.
		unsigned char crc = answer[asize - 1];
		unsigned char ccrc = checksum_add_uint8 (answer, asize - 1, 0x00);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_quit (oceanic_atom2_device_t *device)
{
	// Send the command to the dive computer.
	unsigned char command[4] = {0x6A, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_send (device, command, sizeof (command), NAK);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_atom2_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t *) malloc (sizeof (oceanic_atom2_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base, context, &oceanic_atom2_device_vtable);

	// Set the default values.
	device->port = NULL;
	device->delay = 0;

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (38400 8N1).
	rc = serial_configure (device->port, 38400, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (device->port, 100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (automatically activated
	// by connecting the device), or already in download mode.
	dc_status_t status = oceanic_atom2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	// Override the base class values.
	if (OCEANIC_COMMON_MATCH (device->base.version, aeris_f10_version)) {
		device->base.layout = &aeris_f10_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom1_version)) {
		device->base.layout = &oceanic_atom1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2_version)) {
		if (array_uint16_be (device->base.version + 0x09) >= 0x3349) {
			device->base.layout = &oceanic_atom2a_layout;
		} else {
			device->base.layout = &oceanic_atom2c_layout;
		}
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2a_version)) {
		device->base.layout = &oceanic_atom2a_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2b_version)) {
		device->base.layout = &oceanic_atom2b_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2c_version)) {
		device->base.layout = &oceanic_atom2c_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, tusa_zenair_version)) {
		device->base.layout = &tusa_zenair_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_oc1_version)) {
		device->base.layout = &oceanic_oc1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_oci_version)) {
		device->base.layout = &oceanic_oci_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom3_version)) {
		device->base.layout = &oceanic_atom3_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_vt4_version)) {
		device->base.layout = &oceanic_vt4_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, hollis_tx1_version)) {
		device->base.layout = &hollis_tx1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_veo1_version)) {
		device->base.layout = &oceanic_veo1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_reactpro_version)) {
		device->base.layout = &oceanic_reactpro_layout;
	} else {
		device->base.layout = &oceanic_default_layout;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_close (dc_device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	// Send the quit command.
	oceanic_atom2_quit (device);

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
oceanic_atom2_device_keepalive (dc_device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char command[4] = {0x91, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_atom2_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[PAGESIZE + 1] = {0};
	unsigned char command[2] = {0x84, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer, PAGESIZE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read the package.
		unsigned int number = address / PAGESIZE;
		unsigned char answer[PAGESIZE + 1] = {0};
		unsigned char command[4] = {0xB1,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0};
		dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer));
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer, PAGESIZE);

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Prepare to write the package.
		unsigned int number = address / PAGESIZE;
		unsigned char prepare[4] = {0xB2,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0x00};
		dc_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), NULL, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char command[PAGESIZE + 2] = {0};
		memcpy (command, data, PAGESIZE);
		command[PAGESIZE] = checksum_add_uint8 (command, PAGESIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DC_STATUS_SUCCESS;
}
