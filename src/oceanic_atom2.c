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

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "array.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_atom2_device_vtable.base)

#define VTX        0x4557
#define I750TC     0x455A

#define MAXRETRIES 2
#define MAXDELAY   16
#define INVALID    0xFFFFFFFF

#define CMD_INIT      0xA8
#define CMD_VERSION   0x84
#define CMD_READ1     0xB1
#define CMD_READ8     0xB4
#define CMD_READ16    0xB8
#define CMD_WRITE     0xB2
#define CMD_KEEPALIVE 0x91
#define CMD_QUIT      0x6A

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_atom2_device_t {
	oceanic_common_device_t base;
	dc_serial_t *port;
	unsigned int delay;
	unsigned int bigpage;
	unsigned char cache[256];
	unsigned int cached;
} oceanic_atom2_device_t;

static dc_status_t oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_close (dc_device_t *abstract);

static const oceanic_common_device_vtable_t oceanic_atom2_device_vtable = {
	{
		sizeof(oceanic_atom2_device_t),
		DC_FAMILY_OCEANIC_ATOM2,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		oceanic_atom2_device_read, /* read */
		oceanic_atom2_device_write, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		NULL, /* timesync */
		oceanic_atom2_device_close /* close */
	},
	oceanic_common_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_version_t aeris_f10_version[] = {
	{"FREEWAER \0\0 512K"},
	{"OCEANF10 \0\0 512K"},
	{"MUNDIAL R\0\0 512K"},
};

static const oceanic_common_version_t aeris_f11_version[] = {
	{"AERISF11 \0\0 1024"},
	{"OCEANF11 \0\0 1024"},
};

static const oceanic_common_version_t oceanic_atom1_version[] = {
	{"ATOM rev\0\0  256K"},
};

static const oceanic_common_version_t oceanic_atom2_version[] = {
	{"2M ATOM r\0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2a_version[] = {
	{"MANTA  R\0\0  512K"},
	{"INSIGHT2 \0\0 512K"},
	{"OCEVEO30 \0\0 512K"},
	{"ATMOSAI R\0\0 512K"},
	{"PROPLUS2 \0\0 512K"},
	{"OCEGEO20 \0\0 512K"},
	{"OCE GEO R\0\0 512K"},
	{"AQUAI200 \0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2b_version[] = {
	{"ELEMENT2 \0\0 512K"},
	{"OCEVEO20 \0\0 512K"},
	{"TUSAZEN \0\0  512K"},
	{"AQUAI300 \0\0 512K"},
	{"HOLLDG03 \0\0 512K"},
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
};

static const oceanic_common_version_t sherwood_wisdom_version[] = {
	{"WISDOM R\0\0  512K"},
};

static const oceanic_common_version_t oceanic_proplus3_version[] = {
	{"PROPLUS3 \0\0 512K"},
};

static const oceanic_common_version_t tusa_zenair_version[] = {
	{"TUZENAIR \0\0 512K"},
	{"AMPHOSSW \0\0 512K"},
	{"AMPHOAIR \0\0 512K"},
	{"VOYAGE2G \0\0 512K"},
};

static const oceanic_common_version_t oceanic_oc1_version[] = {
	{"OCWATCH R\0\0 1024"},
	{"OC1WATCH \0\0 1024"},
	{"OCSWATCH \0\0 1024"},
	{"AQUAI550 \0\0 1024"},
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
	{"SWVISION \0\0 1024"},
	{"XPSUBAIR \0\0 1024"},
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

static const oceanic_common_version_t aeris_a300cs_version[] = {
	{"AER300CS \0\0 2048"},
	{"OCEANVTX \0\0 2048"},
	{"AQUAI750 \0\0 2048"},
};

static const oceanic_common_version_t aqualung_i450t_version[] = {
	{"AQUAI450 \0\0 2048"},
};

static const oceanic_common_layout_t aeris_f10_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	0x0D80, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	2, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_f11_layout = {
	0x20000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	0x0D80, /* rb_profile_begin */
	0x20000, /* rb_profile_end */
	0, /* pt_mode_global */
	3, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom1_layout = {
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
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t sherwood_wisdom_layout = {
	0xFFF0, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03D0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_proplus3_layout = {
	0x10000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03E0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
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
	1, /* pt_mode_logbook */
	1, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_a300cs_layout = {
	0x40000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0900, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x1000, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aqualung_i450t_layout = {
	0x40000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x1400, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static dc_status_t
oceanic_atom2_packet (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (device->delay) {
		dc_serial_sleep (device->port, device->delay);
	}

	// Send the command to the dive computer.
	status = dc_serial_write (device->port, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Get the correct ACK byte.
	unsigned int ack = ACK;
	if (command[0] == CMD_INIT || command[0] == CMD_QUIT) {
		ack = NAK;
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = 0;
	status = dc_serial_read (device->port, &response, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the response of the dive computer.
	if (response != ack) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	if (asize) {
		// Receive the answer of the dive computer.
		status = dc_serial_read (device->port, answer, asize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the checksum of the answer.
		unsigned short crc, ccrc;
		if (crc_size == 2) {
			crc = array_uint16_le (answer + asize - 2);
			ccrc = checksum_add_uint16 (answer, asize - 2, 0x0000);
		} else {
			crc = answer[asize - 1];
			ccrc = checksum_add_uint8 (answer, asize - 1, 0x00);
		}
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_atom2_packet (device, command, csize, answer, asize, crc_size)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Increase the inter packet delay.
		if (device->delay < MAXDELAY)
			device->delay++;

		// Delay the next attempt.
		dc_serial_sleep (device->port, 100);
		dc_serial_purge (device->port, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_quit (oceanic_atom2_device_t *device)
{
	// Send the command to the dive computer.
	unsigned char command[4] = {CMD_QUIT, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_atom2_device_open (dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceanic_atom2_device_t *) dc_device_allocate (context, &oceanic_atom2_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Set the default values.
	device->port = NULL;
	device->delay = 0;
	device->bigpage = 1; // no big pages
	device->cached = INVALID;
	memset(device->cache, 0, sizeof(device->cache));

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Get the correct baudrate.
	unsigned int baudrate = 38400;
	if (model == VTX || model == I750TC) {
		baudrate = 115200;
	}

	// Set the serial communication protocol (38400 8N1).
	status = dc_serial_configure (device->port, baudrate, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_serial_set_timeout (device->port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_serial_sleep (device->port, 100);

	// Set the DTR/RTS lines.
	dc_serial_set_dtr(device->port, 1);
	dc_serial_set_rts(device->port, 1);

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (automatically activated
	// by connecting the device), or already in download mode.
	status = oceanic_atom2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Override the base class values.
	if (OCEANIC_COMMON_MATCH (device->base.version, aeris_f10_version)) {
		device->base.layout = &aeris_f10_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aeris_f11_version)) {
		device->base.layout = &aeris_f11_layout;
		device->bigpage = 8;
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
	} else if (OCEANIC_COMMON_MATCH (device->base.version, sherwood_wisdom_version)) {
		device->base.layout = &sherwood_wisdom_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_proplus3_version)) {
		device->base.layout = &oceanic_proplus3_layout;
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
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aeris_a300cs_version)) {
		device->base.layout = &aeris_a300cs_layout;
		device->bigpage = 16;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aqualung_i450t_version)) {
		device->base.layout = &aqualung_i450t_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_default_version)) {
		device->base.layout = &oceanic_default_layout;
	} else {
		WARNING (context, "Unsupported device detected!");
		device->base.layout = &oceanic_default_layout;
		if (memcmp(device->base.version + 12, "256K", 4) == 0) {
			device->base.layout = &oceanic_atom1_layout;
		} else if (memcmp(device->base.version + 12, "512K", 4) == 0) {
			device->base.layout = &oceanic_default_layout;
		} else if (memcmp(device->base.version + 12, "1024", 4) == 0) {
			device->base.layout = &oceanic_oc1_layout;
		} else if (memcmp(device->base.version + 12, "2048", 4) == 0) {
			device->base.layout = &hollis_tx1_layout;
		} else {
			device->base.layout = &oceanic_default_layout;
		}
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
oceanic_atom2_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the quit command.
	rc = oceanic_atom2_quit (device);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
oceanic_atom2_device_keepalive (dc_device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char command[4] = {CMD_KEEPALIVE, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
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
	unsigned char command[2] = {CMD_VERSION, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
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

	// Pick the correct read command and number of checksum bytes.
	unsigned char read_cmd = 0x00;
	unsigned int crc_size = 0;
	switch (device->bigpage) {
	case 1:
		read_cmd = CMD_READ1;
		crc_size = 1;
		break;
	case 8:
		read_cmd = CMD_READ8;
		crc_size = 1;
		break;
	case 16:
		read_cmd = CMD_READ16;
		crc_size = 2;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Pick the best pagesize to use.
	unsigned int pagesize = device->bigpage * PAGESIZE;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		unsigned int page = address / pagesize;
		if (page != device->cached) {
			// Read the package.
			unsigned int number = page * device->bigpage; // This is always PAGESIZE, even in big page mode.
			unsigned char answer[256 + 2] = {0};          // Maximum we support for the known commands.
			unsigned char command[4] = {read_cmd,
					(number >> 8) & 0xFF, // high
					(number     ) & 0xFF, // low
					0};
			dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer,  pagesize + crc_size, crc_size);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Cache the page.
			memcpy (device->cache, answer, pagesize);
			device->cached = page;
		}

		unsigned int offset = address % pagesize;
		unsigned int length = pagesize - offset;
		if (nbytes + length > size)
			length = size - nbytes;

		memcpy (data, device->cache + offset, length);

		nbytes += length;
		address += length;
		data += length;
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

	// Invalidate the cache.
	device->cached = INVALID;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Prepare to write the package.
		unsigned int number = address / PAGESIZE;
		unsigned char prepare[4] = {CMD_WRITE,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0x00};
		dc_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char command[PAGESIZE + 2] = {0};
		memcpy (command, data, PAGESIZE);
		command[PAGESIZE] = checksum_add_uint8 (command, PAGESIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DC_STATUS_SUCCESS;
}
