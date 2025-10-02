/*
 * libdivecomputer
 *
 * Copyright (C) 2022 Jef Driesen
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

#include "seac_screen.h"
#include "seac_screen_common.h"
#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "rbstream.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &seac_screen_device_vtable)

#define MAXRETRIES 4

#define START     0x55
#define ACK       0x09
#define NAK       0x30

#define ERR_INVALID_CMD    0x02
#define ERR_INVALID_LENGTH 0x03
#define ERR_INVALID_DATA   0x04
#define ERR_BATTERY_LOW    0x05
#define ERR_BUSY           0x06

#define SZ_MAXCMD   8
#define SZ_MAXRSP   SZ_READ

#define CMD_HWINFO  0x1833
#define CMD_SWINFO  0x1834

// Screen
#define CMD_SCREEN_RANGE   0x1840
#define CMD_SCREEN_ADDRESS 0x1841
#define CMD_SCREEN_READ    0x1842

// Tablet
#define CMD_TABLET_RANGE   0x1850
#define CMD_TABLET_ADDRESS 0x1851
#define CMD_TABLET_READ    0x1852

#define SZ_HWINFO  256
#define SZ_SWINFO  256
#define SZ_RANGE   8
#define SZ_ADDRESS 4
#define SZ_READ    2048

#define FP_OFFSET   0
#define FP_SIZE     4

#define RB_PROFILE_DISTANCE(a,b,l) ringbuffer_distance (a, b, DC_RINGBUFFER_FULL, l->rb_profile_begin, l->rb_profile_end)
#define RB_PROFILE_INCR(a,b,l)     ringbuffer_increment (a, b, l->rb_profile_begin, l->rb_profile_end)

#define ACTION 0x01
#define SCREEN 0x02
#define TABLET 0x10

typedef struct seac_screen_commands_t {
	unsigned short range;
	unsigned short address;
	unsigned short read;
} seac_screen_commands_t;

typedef struct seac_screen_layout_t {
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
} seac_screen_layout_t;

typedef struct seac_screen_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	const seac_screen_commands_t *cmds;
	const seac_screen_layout_t *layout;
	unsigned int fingerprint;
	unsigned char info[SZ_HWINFO + SZ_SWINFO];
} seac_screen_device_t;

static dc_status_t seac_screen_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t seac_screen_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t seac_screen_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t seac_screen_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t seac_screen_device_vtable = {
	sizeof(seac_screen_device_t),
	DC_FAMILY_SEAC_SCREEN,
	seac_screen_device_set_fingerprint, /* set_fingerprint */
	seac_screen_device_read, /* read */
	NULL, /* write */
	seac_screen_device_dump, /* dump */
	seac_screen_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL, /* close */
};

static const seac_screen_commands_t cmds_screen = {
	CMD_SCREEN_RANGE,
	CMD_SCREEN_ADDRESS,
	CMD_SCREEN_READ,
};

static const seac_screen_commands_t cmds_tablet = {
	CMD_TABLET_RANGE,
	CMD_TABLET_ADDRESS,
	CMD_TABLET_READ,
};

static const seac_screen_layout_t layout_screen = {
	0x010000, /* rb_profile_begin */
	0x200000, /* rb_profile_end */
};

static const seac_screen_layout_t layout_tablet = {
	0x0A0000, /* rb_profile_begin */
	0x200000, /* rb_profile_end */
};

static dc_status_t
seac_screen_send (seac_screen_device_t *device, unsigned short cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned short crc = 0;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (size > SZ_MAXCMD)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	unsigned len = size + 6;
	unsigned char packet[SZ_MAXCMD + 7] = {
		START,
		(len >> 8) & 0xFF,
		(len     ) & 0xFF,
		(cmd >> 8) & 0xFF,
		(cmd     ) & 0xFF,
	};
	if (size) {
		memcpy (packet + 5, data, size);
	}
	crc = checksum_crc16_ccitt (packet, size + 5, 0xFFFF, 0x0000);
	packet[size +  5] = (crc >> 8) & 0xFF;
	packet[size +  6] = (crc     ) & 0xFF;

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, size + 7, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_receive (seac_screen_device_t *device, unsigned short cmd, unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[SZ_MAXRSP + 8] = {0};

	// Read the packet start byte.
	while (1) {
		status = dc_iostream_read (device->iostream, packet + 0, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet start byte.");
			return status;
		}

		if (packet[0] == START)
			break;

		WARNING (abstract->context, "Unexpected packet header byte (%02x).", packet[0]);
	}

	// Read the packet length.
	status = dc_iostream_read (device->iostream, packet + 1, 2, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet length.");
		return status;
	}

	// Verify the length.
	unsigned int length = array_uint16_be (packet + 1);
	if (length < 7 || length + 1 > sizeof(packet)) {
		ERROR (abstract->context, "Unexpected packet length (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Read the packet payload.
	status = dc_iostream_read (device->iostream, packet + 3, length - 2, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet payload.");
		return status;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be (packet + 1 + length - 2);
	unsigned short ccrc = checksum_crc16_ccitt (packet, 1 + length - 2, 0xFFFF, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected packet checksum (%04x %04x).", crc, ccrc);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the command response.
	unsigned int rsp = array_uint16_be (packet + 3);
	if (rsp != cmd) {
		ERROR (abstract->context, "Unexpected command response (%04x).", rsp);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the ACK/NAK byte.
	unsigned int type = packet[1 + length - 3];
	if (type != ACK && type != NAK) {
		ERROR (abstract->context, "Unexpected ACK/NAK byte (%02x).", type);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the length of the packet.
	unsigned int expected = (type == ACK ? size : 1) + 7;
	if (length != expected) {
		ERROR (abstract->context, "Unexpected packet length (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Get the error code from a NAK packet.
	if (type == NAK) {
		unsigned int errcode = packet[5];
		ERROR (abstract->context, "Received NAK packet with error code %02x.", errcode);
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, packet + 5, length - 7);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_packet (seac_screen_device_t *device, unsigned int cmd, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	status = seac_screen_send (device, cmd, data, size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	status = seac_screen_receive (device, cmd, answer, asize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the response.");
		return status;
	}

	return status;
}

static dc_status_t
seac_screen_transfer (seac_screen_device_t *device, unsigned int cmd, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = seac_screen_packet (device, cmd, data, size, answer, asize)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Discard any garbage bytes.
		dc_iostream_sleep (device->iostream, 100);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
seac_screen_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	seac_screen_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (seac_screen_device_t *) dc_device_allocate (context, &seac_screen_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->cmds = NULL;
	device->layout = NULL;
	device->fingerprint = 0;
	memset (device->info, 0, sizeof (device->info));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Wakeup the device.
	const unsigned char init[] = {0x61};
	dc_iostream_write (device->iostream, init, sizeof (init), NULL);

	// Read the hardware info.
	status = seac_screen_transfer (device, CMD_HWINFO, NULL, 0, device->info, SZ_HWINFO);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the hardware info.");
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Hardware", device->info, SZ_HWINFO);

	// Read the software info.
	status = seac_screen_transfer (device, CMD_SWINFO, NULL, 0, device->info + SZ_HWINFO, SZ_SWINFO);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the software info.");
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Software", device->info + SZ_HWINFO, SZ_SWINFO);

	unsigned int model = array_uint32_le (device->info + 4);
	if (model == TABLET) {
		device->cmds = &cmds_tablet;
		device->layout = &layout_tablet;
	} else {
		device->cmds = &cmds_screen;
		device->layout = &layout_screen;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
seac_screen_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	seac_screen_device_t *device = (seac_screen_device_t *) abstract;

	if (size && size != FP_SIZE)
		return DC_STATUS_INVALIDARGS;

	if (size) {
		device->fingerprint = array_uint32_le (data);
	} else {
		device->fingerprint = 0;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
seac_screen_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	seac_screen_device_t *device = (seac_screen_device_t *) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Maximum payload size.
		unsigned int len = size - nbytes;
		if (len > SZ_READ)
			len = SZ_READ;

		// Read the data packet.
		// Regardless of the requested payload size, the packet size is always
		// the maximum size. The remainder of the packet is padded with zeros.
		const unsigned char params[] = {
			(address >> 24) & 0xFF,
			(address >> 16) & 0xFF,
			(address >>  8) & 0xFF,
			(address      ) & 0xFF,
			(len >> 24) & 0xFF,
			(len >> 16) & 0xFF,
			(len >>  8) & 0xFF,
			(len      ) & 0xFF,
		};
		unsigned char packet[SZ_READ] = {0};
		const unsigned int packetsize = device->cmds->read == CMD_TABLET_READ ? len : sizeof(packet);
		status = seac_screen_transfer (device, device->cmds->read, params, sizeof(params), packet, packetsize);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the read command.");
			return status;
		}

		// Copy only the payload bytes.
		memcpy (data, packet, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return status;
}

static dc_status_t
seac_screen_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	seac_screen_device_t *device = (seac_screen_device_t *) abstract;
	const seac_screen_layout_t *layout = device->layout;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint32_le (device->info + 4);
	if (devinfo.model == TABLET) {
		devinfo.firmware = array_uint32_le (device->info + 0x114);
	} else {
		devinfo.firmware = array_uint32_le (device->info + 0x11C);
	}
	devinfo.serial = array_uint32_le (device->info + 0x10);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->info;
	vendor.size = sizeof(device->info);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, layout->rb_profile_end - layout->rb_profile_begin)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, layout->rb_profile_begin, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_READ);
}

static dc_status_t
seac_screen_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	seac_screen_device_t *device = (seac_screen_device_t *) abstract;
	const seac_screen_layout_t *layout = device->layout;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = layout->rb_profile_end - layout->rb_profile_begin;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint32_le (device->info + 4);
	if (devinfo.model == TABLET) {
		devinfo.firmware = array_uint32_le (device->info + 0x114);
	} else {
		devinfo.firmware = array_uint32_le (device->info + 0x11C);
	}
	devinfo.serial = array_uint32_le (device->info + 0x010);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->info;
	vendor.size = sizeof(device->info);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read the range of the available dive numbers.
	unsigned char range[SZ_RANGE] = {0};
	status = seac_screen_transfer (device, device->cmds->range, NULL, 0, range, sizeof(range));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the range command.");
		goto error_exit;
	}

	// Extract the first and last dive number.
	unsigned int first = array_uint32_be (range + 0);
	unsigned int last  = array_uint32_be (range + 4);
	if (first > last) {
		ERROR (abstract->context, "Invalid dive numbers (%u %u).", first, last);
		status = DC_STATUS_DATAFORMAT;
		goto error_exit;
	}

	// Calculate the number of dives.
	unsigned int ndives = last - first + 1;

	// Check the fingerprint.
	if (device->fingerprint >= last) {
		ndives = 0;
	} else if (device->fingerprint >= first) {
		first = device->fingerprint + 1;
		ndives = last - first + 1;
	}

	// Update and emit a progress event.
	progress.current += SZ_RANGE;
	progress.maximum += SZ_RANGE + ndives * SZ_ADDRESS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Exit if no dives to download.
	if (ndives == 0) {
		goto error_exit;
	}

	// Allocate memory for the dive addresses.
	unsigned int *address = (unsigned int *) malloc (ndives * sizeof (unsigned int));
	if (address == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Read the address of each dive in reverse order (most recent first).
	unsigned int eop = 0;
	unsigned int previous = 0;
	unsigned int begin = 0;
	unsigned int count = 0;
	unsigned int skip = 0;
	unsigned int rb_profile_size = 0;
	unsigned int remaining = layout->rb_profile_end - layout->rb_profile_begin;
	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int number = last - i;

		// Read the dive address.
		const unsigned char cmd_address[] = {
			(number >> 24) & 0xFF,
			(number >> 16) & 0xFF,
			(number >>  8) & 0xFF,
			(number      ) & 0xFF,
		};
		unsigned char rsp_address[SZ_ADDRESS] = {0};
		status = seac_screen_transfer (device, device->cmds->address, cmd_address, sizeof(cmd_address), rsp_address, sizeof(rsp_address));
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive address.");
			goto error_free_logbook;
		}

		// Update and emit a progress event.
		progress.current += SZ_ADDRESS;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		// Get the dive address.
		address[i] = array_uint32_be (rsp_address);
		if (address[i] < layout->rb_profile_begin || address[i] >= layout->rb_profile_end) {
			ERROR (abstract->context, "Invalid ringbuffer pointer (0x%08x).", address[i]);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_logbook;
		}

		// Get the end-of-profile pointer.
		if (eop == 0) {
			// Read the dive header.
			unsigned char header[SZ_HEADER] = {0};
			status = seac_screen_device_read (abstract, address[i], header, sizeof(header));
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to read the dive header.");
				goto error_free_logbook;
			}

			// Update and emit a progress event.
			progress.current += sizeof(header);
			progress.maximum += sizeof(header);
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			// Check the header records.
			unsigned int isvalid = 1;
			for (unsigned int j = 0; j < 2; ++j) {
				unsigned int type = j == 0 ? HEADER1 : HEADER2;
				if (!seac_screen_record_isvalid (abstract->context,
					header + j * SZ_HEADER / 2, SZ_HEADER / 2,
					type, number)) {
					WARNING (abstract->context, "Invalid header record %u.", j);
					isvalid = 0;
				}
			}

			// For dives with an invalid header, the number of samples in the
			// header is not guaranteed to be valid. Discard the entire dive
			// instead and take its start address as the end of the profile.
			if (!isvalid) {
				WARNING (abstract->context, "Unable to locate the end of the profile.");
				eop = previous = address[i];
				begin = 1;
				skip++;
				continue;
			}

			// Get the number of samples.
			unsigned int nsamples = array_uint32_le (header + 0x44);
			unsigned int nbytes = SZ_HEADER + nsamples * SZ_SAMPLE;

			// Calculate the end of the profile.
			eop = previous = RB_PROFILE_INCR (address[i], nbytes, layout);
		}

		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (address[i], previous, layout);

		// Check for the end of the ringbuffer.
		if (length > remaining) {
			WARNING (abstract->context, "Reached the end of the ringbuffer.");
			skip++;
			break;
		}

		// Update the total profile size.
		rb_profile_size += length;

		// Move to the start of the current dive.
		remaining -= length;
		previous = address[i];
		count++;
	}

	// Update and emit a progress event.
	progress.maximum -= (ndives - count - skip) * SZ_ADDRESS +
		((layout->rb_profile_end - layout->rb_profile_begin) - rb_profile_size);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Exit if no dives to download.
	if (count == 0) {
		goto error_free_logbook;
	}

	// Allocate memory for the profile data.
	unsigned char *profile = (unsigned char *) malloc (rb_profile_size);
	if (profile == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_free_logbook;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	status = dc_rbstream_new (&rbstream, abstract, SZ_READ, SZ_READ, layout->rb_profile_begin, layout->rb_profile_end, eop, DC_RBSTREAM_BACKWARD);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		goto error_free_profile;
	}

	previous = eop;
	unsigned int offset = rb_profile_size;
	for (unsigned int i = 0; i < count; ++i) {
		unsigned int idx = begin + i;
		unsigned int number = last - idx;

		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (address[idx], previous, layout);

		// Move to the start of the current dive.
		offset -= length;
		previous = address[idx];

		// Read the dive.
		status = dc_rbstream_read (rbstream, &progress, profile + offset, length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			goto error_free_rbstream;
		}

		// Check the minimum header length.
		if (length < SZ_HEADER) {
			ERROR (abstract->context, "Unexpected dive length (%u).", length);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rbstream;
		}

		// Check the header records.
		unsigned int isvalid = 1;
		for (unsigned int j = 0; j < 2; ++j) {
			unsigned int type = j == 0 ? HEADER1 : HEADER2;
			if (!seac_screen_record_isvalid (abstract->context,
				profile + offset + j * SZ_HEADER / 2, SZ_HEADER / 2,
				type, number)) {
				WARNING (abstract->context, "Invalid header record %u.", j);
				isvalid = 0;
			}
		}

		// Get the number of samples.
		// The actual size of the dive, based on the number of samples, can
		// sometimes be smaller than the maximum length. In that case, the
		// remainder of the data is padded with 0xFF bytes.
		unsigned int nsamples = 0;
		unsigned int nbytes = 0;
		if (isvalid) {
			nsamples = array_uint32_le (profile + offset + 0x44);
			nbytes = SZ_HEADER + nsamples * SZ_SAMPLE;
		} else {
			WARNING (abstract->context, "Unable to locate the padding bytes.");
			nbytes = length;
		}

		if (nbytes > length) {
			ERROR (abstract->context, "Unexpected dive length (%u %u).", nbytes, length);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rbstream;
		}

		if (callback && !callback (profile + offset, nbytes, profile + offset + FP_OFFSET, FP_SIZE, userdata)) {
			break;
		}
	}

error_free_rbstream:
	dc_rbstream_free (rbstream);
error_free_profile:
	free (profile);
error_free_logbook:
	free (address);
error_exit:
	return status;
}
