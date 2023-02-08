/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Janice McLaughlin
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

#include <libdivecomputer/ble.h>

#include "pelagic_i330r.h"
#include "oceanic_common.h"

#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "rbstream.h"
#include "checksum.h"
#include "array.h"

#define UNDEFINED 0

#define STARTBYTE  0xCD

#define FLAG_NONE    0x00
#define FLAG_REQUEST 0x40
#define FLAG_DATA    0x80
#define FLAG_LAST    0xC0

#define CMD_ACCESS_REQUEST       0xFA
#define CMD_ACCESS_CODE          0xFB
#define CMD_AUTHENTICATION       0x97
#define CMD_WAKEUP_RDONLY        0x21
#define CMD_WAKEUP_RDWR          0x22
#define CMD_READ_HW_CAL          0x27
#define CMD_READ_A2D             0x25
#define CMD_READ_DEVICE_REC      0x31
#define CMD_READ_GEN_SET         0x29
#define CMD_READ_EXFLASHMAP      0x2F
#define CMD_READ_FLASH           0x0D

#define RSP_READY 1
#define RSP_DONE  2

#define MAXPACKET 255

#define MAXPASSCODE 6

typedef struct pelagic_i330r_device_t {
	oceanic_common_device_t base;
	dc_iostream_t *iostream;
	unsigned char accesscode[16];
	unsigned char id[16];
	unsigned char hwcal[256];
	unsigned char flashmap[256];
	unsigned int model;
} pelagic_i330r_device_t;

static dc_status_t pelagic_i330r_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t pelagic_i330r_device_devinfo (dc_device_t *abstract, dc_event_progress_t *progress);
static dc_status_t pelagic_i330r_device_pointers (dc_device_t *abstract, dc_event_progress_t *progress, unsigned int *rb_logbook_begin, unsigned int *rb_logbook_end, unsigned int *rb_profile_begin, unsigned int *rb_profile_end);

static const oceanic_common_device_vtable_t pelagic_i330r_device_vtable = {
	{
		sizeof(pelagic_i330r_device_t),
		DC_FAMILY_PELAGIC_I330R,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		pelagic_i330r_device_read, /* read */
		NULL, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		NULL, /* timesync */
		NULL /* close */
	},
	pelagic_i330r_device_devinfo,
	pelagic_i330r_device_pointers,
	oceanic_common_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_layout_t pelagic_i330r = {
	0x00400000, /* memsize */
	0, /* highmem */
	UNDEFINED, /* cf_devinfo */
	UNDEFINED, /* cf_pointers */
	0x00102000, /* rb_logbook_begin */
	0x00106000, /* rb_logbook_end */
	64, /* rb_logbook_entry_size */
	0, /* rb_logbook_direction */
	0x0010A000, /* rb_profile_begin */
	0x00400000, /* rb_profile_end */
	1, /* pt_mode_global */
	4, /* pt_mode_logbook */
	UNDEFINED, /* pt_mode_serial */
};

static const oceanic_common_layout_t pelagic_dsx = {
	0x02000000, /* memsize */
	0, /* highmem */
	UNDEFINED, /* cf_devinfo */
	UNDEFINED, /* cf_pointers */
	0x00800000, /* rb_logbook_begin */
	0x00880000, /* rb_logbook_end */
	512, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x01000000, /* rb_profile_begin */
	0x02000000, /* rb_profile_end */
	1, /* pt_mode_global */
	4, /* pt_mode_logbook */
	UNDEFINED /* pt_mode_serial */
};

static unsigned char
checksum (const unsigned char data[], unsigned int size)
{
	unsigned int csum = 0;
	for (unsigned int i = 0; i < size; i++) {
		unsigned int a = csum ^ data[i];
		unsigned int b = (a >> 7) ^ ((a >> 4) ^ a);
		csum = ((b << 4) & 0xFF) ^ ((b << 1) & 0xFF);
	}
	return csum & 0xFF;
}

static dc_status_t
pelagic_i330r_send (pelagic_i330r_device_t *device, unsigned char cmd, unsigned char flag, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > MAXPACKET) {
		ERROR (abstract->context, "Packet payload is too large (%u).", size);
		return DC_STATUS_INVALIDARGS;
	}

	unsigned char packet[MAXPACKET + 5] = {
		STARTBYTE,
		flag,
		cmd,
		0,
		size
	};
	if (size) {
		memcpy(packet + 5, data, size);
	}
	packet[3] = checksum (packet, size + 5);

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, size + 5, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
pelagic_i330r_recv (pelagic_i330r_device_t *device, unsigned char cmd, unsigned char data[], unsigned int size, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET + 5] = {0};
	unsigned int errcode = 0;

	unsigned int nbytes = 0;
	while (1) {
		// Read the data packet.
		size_t transferred = 0;
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), &transferred);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the data packet.");
			return status;
		}

		// Verify the minimum packet size.
		if (transferred < 5) {
			ERROR (abstract->context, "Invalid packet length (" DC_PRINTF_SIZE ").", transferred);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the start byte.
		if (packet[0] != STARTBYTE) {
			ERROR (abstract->context, "Unexpected packet start byte (%02x).", packet[0]);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the command byte.
		if (packet[2] != cmd) {
			ERROR (abstract->context, "Unexpected packet command byte (%02x).", packet[2]);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the length byte.
		unsigned int length = packet[4];
		if (length + 5 > transferred) {
			ERROR (abstract->context, "Invalid packet length (%u).", length);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the checksum.
		unsigned char crc = packet[3]; packet[3] = 0;
		unsigned char ccrc = checksum (packet, length + 5);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected packet checksum (%02x %02x).", crc, ccrc);
			return DC_STATUS_PROTOCOL;
		}

		// Check the flag byte for the last packet.
		unsigned char flag = packet[1];
		if ((flag & FLAG_LAST) == FLAG_LAST) {
			// The last packet (typically 2 bytes) does not get appended!
			if (length) {
				errcode = packet[5];
			}
			break;
		}

		// Append the payload data to the output buffer. If the output
		// buffer is too small, the error is not reported immediately
		// but delayed until all packets have been received.
		if (nbytes < size) {
			unsigned int n = length;
			if (nbytes + n > size) {
				n = size - nbytes;
			}
			memcpy (data + nbytes, packet + 5, n);
		}
		nbytes += length;
	}

	// Verify the expected number of bytes.
	if (nbytes != size) {
		ERROR (abstract->context, "Unexpected number of bytes received (%u %u).", nbytes, size);
		return DC_STATUS_PROTOCOL;
	}

	if (errorcode) {
		*errorcode = errcode;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
pelagic_i330r_transfer (pelagic_i330r_device_t *device, unsigned char cmd, unsigned char flag, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize, unsigned int response)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int errorcode = 0;

	status = pelagic_i330r_send (device, cmd, flag, data, size);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = pelagic_i330r_recv (device, cmd, answer, asize, &errorcode);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (errorcode != response) {
		ERROR (abstract->context, "Unexpected response code (%u)", errorcode);
		return DC_STATUS_PROTOCOL;
	}

	return status;
}

static dc_status_t
pelagic_i330r_init_accesscode (pelagic_i330r_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	const unsigned char zero[9] = {0};
	status = pelagic_i330r_transfer (device, CMD_ACCESS_REQUEST, FLAG_REQUEST, zero, sizeof(zero), NULL, 0, RSP_READY);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = pelagic_i330r_transfer (device, CMD_ACCESS_REQUEST, FLAG_DATA, device->accesscode, sizeof(device->accesscode), NULL, 0, RSP_DONE);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return status;
}

static dc_status_t
pelagic_i330r_init_passcode (pelagic_i330r_device_t *device, const char *pincode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char passcode[MAXPASSCODE] = {0};

	// Check the maximum length.
	size_t len = pincode ? strlen (pincode) : 0;
	if (len > sizeof(passcode)) {
		ERROR (abstract->context, "Invalid pincode length (" DC_PRINTF_SIZE ").", len);
		return DC_STATUS_INVALIDARGS;
	}

	// Convert to binary number.
	unsigned int offset = sizeof(passcode) - len;
	for (unsigned int i = 0; i < len; i++) {
		unsigned char c = pincode[i];
		if (c < '0' || c > '9') {
			ERROR (abstract->context, "Invalid pincode character (%c).", c);
			return DC_STATUS_INVALIDARGS;
		}
		passcode[offset + i] = c - '0';
	}

	const unsigned char zero[9] = {0};
	status = pelagic_i330r_transfer (device, CMD_ACCESS_CODE, FLAG_REQUEST, zero, sizeof(zero), NULL, 0, RSP_READY);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = pelagic_i330r_transfer (device, CMD_ACCESS_CODE, FLAG_DATA, passcode, sizeof(passcode), device->accesscode, sizeof(device->accesscode), RSP_DONE);
	if (status != DC_STATUS_SUCCESS)
		return status;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Access code", device->accesscode, sizeof(device->accesscode));

	return status;
}

static dc_status_t
pelagic_i330r_init_handshake (pelagic_i330r_device_t *device, unsigned int readwrite)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	const unsigned char cmd = readwrite ? CMD_WAKEUP_RDWR : CMD_WAKEUP_RDONLY;

	const unsigned char args[9] = {0, 0, 0, 0, 0x0C, 0, 0, 0, 0};
	status = pelagic_i330r_transfer (device, cmd, FLAG_REQUEST, args, sizeof(args), device->id, sizeof(device->id), RSP_DONE);
	if (status != DC_STATUS_SUCCESS)
		return status;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "ID", device->id, sizeof(device->id));

	device->model = array_uint16_be (device->id + 12);

	return status;
}

static dc_status_t
pelagic_i330r_init_auth (pelagic_i330r_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	const unsigned char args[2][9] = {
		{0xFF, 0xFF, 0xFF, 0xFF}, // DSX
		{0x37, 0x30, 0x31, 0x55}, // I330R
	};
	unsigned int args_idx = device->model == DSX ? 0 : 1;
	status = pelagic_i330r_transfer (device, CMD_AUTHENTICATION, FLAG_REQUEST, args[args_idx], sizeof(args[args_idx]), NULL, 0, RSP_READY);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return status;
}

static dc_status_t
pelagic_i330r_init (pelagic_i330r_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Get the bluetooth access code.
	status = dc_iostream_ioctl (device->iostream, DC_IOCTL_BLE_GET_ACCESSCODE, device->accesscode, sizeof(device->accesscode));
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR (abstract->context, "Failed to get the access code.");
		return status;
	}

	if (array_isequal (device->accesscode, sizeof(device->accesscode), 0)) {
		// Request to display the PIN code.
		status = pelagic_i330r_init_accesscode (device);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to display the PIN code.");
			return status;
		}

		// Get the bluetooth PIN code.
		char pincode[6 + 1] = {0};
		status = dc_iostream_ioctl (device->iostream, DC_IOCTL_BLE_GET_PINCODE, pincode, sizeof(pincode));
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to get the PIN code.");
			return status;
		}

		// Force a null terminated string.
		pincode[sizeof(pincode) - 1] = 0;

		// Request the access code.
		status = pelagic_i330r_init_passcode (device, pincode);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to request the access code.");
			return status;
		}

		// Store the bluetooth access code.
		status = dc_iostream_ioctl (device->iostream, DC_IOCTL_BLE_SET_ACCESSCODE, device->accesscode, sizeof(device->accesscode));
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
			ERROR (abstract->context, "Failed to store the access code.");
			return status;
		}
	}

	// Request access.
	status = pelagic_i330r_init_accesscode (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to request access.");
		return status;
	}

	// Send the wakeup command.
	status = pelagic_i330r_init_handshake (device, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the wakeup command.");
		return status;
	}

	// Send the authentication code.
	status = pelagic_i330r_init_auth (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the authentication code.");
		return status;
	}

	return status;
}

static dc_status_t
pelagic_i330r_download (pelagic_i330r_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	status = pelagic_i330r_transfer (device, cmd, FLAG_REQUEST, data, size, answer, asize, RSP_DONE);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// Verify the checksum
	unsigned short crc = array_uint16_be (answer + asize - 2);
	unsigned short ccrc = checksum_crc16_ccitt (answer, asize - 2, 0xffff, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected data checksum (%04x %04x).", crc, ccrc);
		return DC_STATUS_PROTOCOL;
	}

	return status;
}

dc_status_t
pelagic_i330r_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	pelagic_i330r_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (pelagic_i330r_device_t *) dc_device_allocate (context, &pelagic_i330r_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Override the base class values.
	device->base.multipage = 256;

	// Set the default values.
	device->iostream = iostream;
	memset (device->accesscode, 0, sizeof(device->accesscode));
	memset (device->id, 0, sizeof(device->id));
	memset (device->hwcal, 0, sizeof(device->hwcal));
	memset (device->flashmap, 0, sizeof(device->flashmap));
	device->model = 0;

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Perform the bluetooth authentication.
	status = pelagic_i330r_init (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to perform the bluetooth authentication.");
		goto error_free;
	}

	// Download the calibration data.
	const unsigned char args[9] = {0, 0, 0, 0, 0, 0x01, 0, 0, 0};
	status = pelagic_i330r_download (device, CMD_READ_HW_CAL, args, sizeof(args), device->hwcal, sizeof(device->hwcal));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to download the calibration data.");
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Hwcal", device->hwcal, sizeof(device->hwcal));

	// Download the flash map.
	const unsigned char zero[9] = {0};
	status = pelagic_i330r_download (device, CMD_READ_EXFLASHMAP, zero, sizeof(zero), device->flashmap, sizeof(device->flashmap));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to download the flash map.");
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Flashmap", device->flashmap, sizeof(device->flashmap));

	// Detect the memory layout.
	if (device->model == DSX) {
		device->base.layout = &pelagic_dsx;
	} else {
		device->base.layout = &pelagic_i330r;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
pelagic_i330r_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	pelagic_i330r_device_t *device = (pelagic_i330r_device_t*) abstract;

	unsigned char command[9] = {0};
	array_uint32_le_set(command + 0, address);
	array_uint32_le_set(command + 4, size);

	status = pelagic_i330r_transfer (device, CMD_READ_FLASH, FLAG_NONE, command, sizeof(command), data, size, RSP_DONE);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	return status;
}

static dc_status_t
pelagic_i330r_device_devinfo (dc_device_t *abstract, dc_event_progress_t *progress)
{
	pelagic_i330r_device_t *device = (pelagic_i330r_device_t *) abstract;

	assert (device != NULL);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial =
		bcd2dec (device->hwcal[12]) +
		bcd2dec (device->hwcal[13]) * 100 +
		bcd2dec (device->hwcal[14]) * 10000;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
pelagic_i330r_device_pointers (dc_device_t *abstract, dc_event_progress_t *progress, unsigned int *rb_logbook_begin, unsigned int *rb_logbook_end, unsigned int *rb_profile_begin, unsigned int *rb_profile_end)
{
	pelagic_i330r_device_t *device = (pelagic_i330r_device_t *) abstract;

	assert (device != NULL);
	assert (device->base.layout != NULL);
	assert (rb_logbook_begin != NULL && rb_logbook_end != NULL);
	assert (rb_profile_begin != NULL && rb_profile_end != NULL);

	const oceanic_common_layout_t *layout = device->base.layout;

	// Get the logbook pointers.
	unsigned int rb_logbook_min   = array_uint32_le (device->flashmap + 0x50);
	unsigned int rb_logbook_max   = array_uint32_le (device->flashmap + 0x54);
	unsigned int rb_logbook_first = array_uint32_le (device->flashmap + 0x58);
	unsigned int rb_logbook_last  = array_uint32_le (device->flashmap + 0x5C);
	if (rb_logbook_min != 0 && rb_logbook_max != 0) {
		rb_logbook_max += 1;
	}

	// Get the profile pointers.
	unsigned int rb_profile_min   = array_uint32_le (device->flashmap + 0x70);
	unsigned int rb_profile_max   = array_uint32_le (device->flashmap + 0x74);
	unsigned int rb_profile_first = array_uint32_le (device->flashmap + 0x78);
	unsigned int rb_profile_last  = array_uint32_le (device->flashmap + 0x7C);
	if (rb_profile_min != 0 && rb_profile_max != 0) {
		rb_profile_max += 1;
	}

	// Check the logbook ringbuffer area.
	if (rb_logbook_min != layout->rb_logbook_begin ||
		rb_logbook_max != layout->rb_logbook_end) {
		ERROR (abstract->context, "Unexpected logbook ringbuffer area (%08x %08x)",
			rb_logbook_min, rb_logbook_max);
		return DC_STATUS_DATAFORMAT;
	}

	// Check the profile ringbuffer area.
	if (rb_profile_min != layout->rb_profile_begin ||
		rb_profile_max != layout->rb_profile_end) {
		ERROR (abstract->context, "Unexpected profile ringbuffer area (%08x %08x)",
			rb_profile_min, rb_profile_max);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the begin/end pointers.
	if (device->model == DSX) {
		*rb_logbook_begin = rb_logbook_first;
		*rb_logbook_end   = rb_logbook_last;
	} else {
		*rb_logbook_begin = rb_logbook_min;
		*rb_logbook_end   = rb_logbook_last + 1;
	}
	*rb_profile_begin = rb_profile_first;
	*rb_profile_end   = rb_profile_last;

	return DC_STATUS_SUCCESS;
}
