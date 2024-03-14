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
#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "rbstream.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &seac_screen_device_vtable)

#define MAXRETRIES 4

#define SZ_MAXCMD   8
#define SZ_MAXRSP   SZ_READ

#define CMD_HWINFO  0x1833
#define CMD_SWINFO  0x1834
#define CMD_RANGE   0x1840
#define CMD_ADDRESS 0x1841
#define CMD_READ    0x1842

#define SZ_HWINFO  256
#define SZ_SWINFO  256
#define SZ_RANGE   8
#define SZ_ADDRESS 4
#define SZ_READ    2048

#define SZ_HEADER  128
#define SZ_SAMPLE   64

#define FP_OFFSET   0x0A
#define FP_SIZE     7

#define RB_PROFILE_BEGIN         0x010000
#define RB_PROFILE_END           0x200000
#define RB_PROFILE_SIZE          (RB_PROFILE_END - RB_PROFILE_BEGIN)
#define RB_PROFILE_DISTANCE(a,b) ringbuffer_distance (a, b, DC_RINGBUFFER_FULL, RB_PROFILE_BEGIN, RB_PROFILE_END)
#define RB_PROFILE_INCR(a,d)     ringbuffer_increment (a, d, RB_PROFILE_BEGIN, RB_PROFILE_END)

typedef struct seac_screen_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char info[SZ_HWINFO + SZ_SWINFO];
	unsigned char fingerprint[FP_SIZE];
} seac_screen_device_t;

typedef struct seac_screen_logbook_t {
	unsigned int address;
	unsigned char header[SZ_HEADER];
} seac_screen_logbook_t;

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
		0x55,
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

	// Read the packet header.
	status = dc_iostream_read (device->iostream, packet, 3, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet header.");
		return status;
	}

	// Verify the start byte.
	if (packet[0] != 0x55) {
		ERROR (abstract->context, "Unexpected start byte (%02x).", packet[0]);
		return DC_STATUS_PROTOCOL;
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
	unsigned int misc = packet[1 + length - 3];
	if (rsp != cmd || misc != 0x09) {
		ERROR (abstract->context, "Unexpected command response (%04x %02x).", rsp, misc);
		return DC_STATUS_PROTOCOL;
	}

	if (length - 7 != size) {
		ERROR (abstract->context, "Unexpected packet length (%u).", length);
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
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

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

	// Read the software info.
	status = seac_screen_transfer (device, CMD_SWINFO, NULL, 0, device->info + SZ_HWINFO, SZ_SWINFO);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the software info.");
		goto error_free;
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

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

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
		status = seac_screen_transfer (device, CMD_READ, params, sizeof(params), packet, sizeof(packet));
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

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint32_le (device->info + 0x11C);
	devinfo.serial = array_uint32_le (device->info + 0x10);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->info;
	vendor.size = sizeof(device->info);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, RB_PROFILE_SIZE)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, RB_PROFILE_BEGIN, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_READ);
}

static dc_status_t
seac_screen_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	seac_screen_device_t *device = (seac_screen_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = RB_PROFILE_SIZE;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint32_le (device->info + 0x11C);
	devinfo.serial = array_uint32_le (device->info + 0x010);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->info;
	vendor.size = sizeof(device->info);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read the range of the available dive numbers.
	unsigned char range[SZ_RANGE] = {0};
	status = seac_screen_transfer (device, CMD_RANGE, NULL, 0, range, sizeof(range));
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

	// Update and emit a progress event.
	progress.current += SZ_RANGE;
	progress.maximum += SZ_RANGE + ndives * (SZ_ADDRESS + SZ_HEADER);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the logbook data.
	seac_screen_logbook_t *logbook = (seac_screen_logbook_t *) malloc (ndives * sizeof (seac_screen_logbook_t));
	if (logbook == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Read the header of each dive in reverse order (most recent first).
	unsigned int eop = 0;
	unsigned int previous = 0;
	unsigned int count = 0;
	unsigned int skip = 0;
	unsigned int rb_profile_size = 0;
	unsigned int remaining = RB_PROFILE_SIZE;
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
		status = seac_screen_transfer (device, CMD_ADDRESS, cmd_address, sizeof(cmd_address), rsp_address, sizeof(rsp_address));
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive address.");
			goto error_free_logbook;
		}

		// Get the dive address.
		logbook[i].address = array_uint32_be (rsp_address);
		if (logbook[i].address < RB_PROFILE_BEGIN || logbook[i].address >= RB_PROFILE_END) {
			ERROR (abstract->context, "Invalid ringbuffer pointer (0x%08x).", logbook[i].address);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_logbook;
		}

		// Read the dive header.
		status = seac_screen_device_read (abstract, logbook[i].address, logbook[i].header, SZ_HEADER);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			goto error_free_logbook;
		}

		// Update and emit a progress event.
		progress.current += SZ_ADDRESS + SZ_HEADER;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		// Check the header checksums.
		if (checksum_crc16_ccitt (logbook[i].header, SZ_HEADER / 2, 0xFFFF, 0x0000) != 0 ||
			checksum_crc16_ccitt (logbook[i].header + SZ_HEADER / 2, SZ_HEADER / 2, 0xFFFF, 0x0000) != 0) {
			ERROR (abstract->context, "Unexpected header checksum.");
			status = DC_STATUS_DATAFORMAT;
			goto error_free_logbook;
		}

		// Check the fingerprint.
		if (memcmp (logbook[i].header + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			skip = 1;
			break;
		}

		// Get the number of samples.
		unsigned int nsamples = array_uint32_le (logbook[i].header + 0x44);
		unsigned int nbytes = SZ_HEADER + nsamples * SZ_SAMPLE;

		// Get the end-of-profile pointer.
		if (eop == 0) {
			eop = previous = RB_PROFILE_INCR (logbook[i].address, nbytes);
		}

		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (logbook[i].address, previous);

		// Check for the end of the ringbuffer.
		if (length > remaining) {
			WARNING (abstract->context, "Reached the end of the ringbuffer.");
			skip = 1;
			break;
		}

		// Update the total profile size.
		rb_profile_size += length;

		// Move to the start of the current dive.
		remaining -= length;
		previous = logbook[i].address;
		count++;
	}

	// Update and emit a progress event.
	progress.maximum -= (ndives - count - skip) * (SZ_ADDRESS + SZ_HEADER) +
		(RB_PROFILE_SIZE - rb_profile_size);
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
	status = dc_rbstream_new (&rbstream, abstract, SZ_READ, SZ_READ, RB_PROFILE_BEGIN, RB_PROFILE_END, eop, DC_RBSTREAM_BACKWARD);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		goto error_free_profile;
	}

	previous = eop;
	unsigned int offset = rb_profile_size;
	for (unsigned int i = 0; i < count; ++i) {
		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (logbook[i].address, previous);

		// Move to the start of the current dive.
		offset -= length;
		previous = logbook[i].address;

		// Read the dive.
		status = dc_rbstream_read (rbstream, &progress, profile + offset, length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			goto error_free_rbstream;
		}

		// Check the dive header.
		if (memcmp (profile + offset, logbook[i].header, SZ_HEADER) != 0) {
			ERROR (abstract->context, "Unexpected dive header.");
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rbstream;
		}

		// Get the number of samples.
		// The actual size of the dive, based on the number of samples, can
		// sometimes be smaller than the maximum length. In that case, the
		// remainder of the data is padded with 0xFF bytes.
		unsigned int nsamples = array_uint32_le (logbook[i].header + 0x44);
		unsigned int nbytes = SZ_HEADER + nsamples * SZ_SAMPLE;
		if (nbytes > length) {
			ERROR (abstract->context, "Unexpected dive length (%u %u).", nbytes, length);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rbstream;
		}

		if (callback && !callback (profile + offset, nbytes, profile + offset + FP_OFFSET, sizeof(device->fingerprint), userdata)) {
			break;
		}
	}

error_free_rbstream:
	dc_rbstream_free (rbstream);
error_free_profile:
	free (profile);
error_free_logbook:
	free (logbook);
error_exit:
	return status;
}
