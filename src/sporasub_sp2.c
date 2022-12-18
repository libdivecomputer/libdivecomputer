/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Jef Driesen
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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free

#include "sporasub_sp2.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &sporasub_sp2_device_vtable)

#define SZ_MEMORY 0x10000

#define RB_PROFILE_BEGIN 0x0060
#define RB_PROFILE_END   SZ_MEMORY

#define MAXRETRIES 4
#define MAXPACKET 256

#define HEADER_HI  0xA0
#define HEADER_LO  0xA2
#define TRAILER_HI 0xB0
#define TRAILER_LO 0xB3

#define CMD_VERSION  0x10
#define CMD_READ     0x12
#define CMD_TIMESYNC 0x39

#define SZ_VERSION  23
#define SZ_READ     128

#define SZ_HEADER   32
#define SZ_SAMPLE    4

typedef struct sporasub_sp2_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char version[SZ_VERSION];
	unsigned char fingerprint[6];
} sporasub_sp2_device_t;

static dc_status_t sporasub_sp2_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t sporasub_sp2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t sporasub_sp2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t sporasub_sp2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t sporasub_sp2_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t sporasub_sp2_device_vtable = {
	sizeof(sporasub_sp2_device_t),
	DC_FAMILY_SPORASUB_SP2,
	sporasub_sp2_device_set_fingerprint, /* set_fingerprint */
	sporasub_sp2_device_read, /* read */
	NULL, /* write */
	sporasub_sp2_device_dump, /* dump */
	sporasub_sp2_device_foreach, /* foreach */
	sporasub_sp2_device_timesync, /* timesync */
	NULL /* close */
};

static unsigned int
iceil (unsigned int x, unsigned int n)
{
	// Round up to next higher multiple.
	return ((x + n - 1) / n) * n;
}

static dc_status_t
sporasub_sp2_send (sporasub_sp2_device_t *device, unsigned char command, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > MAXPACKET) {
		return DC_STATUS_INVALIDARGS;
	}

	unsigned int len = size + 1;
	unsigned int csum = checksum_add_uint16 (data, size, command);

	unsigned char packet[MAXPACKET + 9] = {0};
	packet[0] = HEADER_HI;
	packet[1] = HEADER_LO;
	packet[2] = (len >> 8) & 0xFF;
	packet[3] = (len     ) & 0xFF;
	packet[4] = command;
	if (size) {
		memcpy(packet + 5, data, size);
	}
	packet[size + 5] = (csum >> 8) & 0xFF;
	packet[size + 6] = (csum     ) & 0xFF;
	packet[size + 7] = TRAILER_HI;
	packet[size + 8] = TRAILER_LO;

	// Send the command to the device.
	status = dc_iostream_write (device->iostream, packet, size + 9, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sporasub_sp2_receive (sporasub_sp2_device_t *device, unsigned char command, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > MAXPACKET) {
		return DC_STATUS_INVALIDARGS;
	}

	// Receive the answer of the device.
	unsigned char packet[MAXPACKET + 9] = {0};
	status = dc_iostream_read (device->iostream, packet, size + 9, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header and trailer of the packet.
	if (packet[0] != HEADER_HI || packet[1] != HEADER_LO ||
		packet[size + 7] != TRAILER_HI || packet[size + 8] != TRAILER_LO) {
		ERROR (abstract->context, "Unexpected answer header/trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the packet length.
	unsigned int len = array_uint16_be (packet + 2);
	if (len != size + 1) {
		ERROR (abstract->context, "Unexpected packet length.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the command byte.
	if (packet[4] != command) {
		ERROR (abstract->context, "Unexpected answer header/trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_be (packet + size + 5);
	unsigned short ccrc = checksum_add_uint16 (packet + 4, size + 1, 0);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (size) {
		memcpy (data, packet + 5, size);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sporasub_sp2_packet (sporasub_sp2_device_t *device, unsigned char cmd, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command to the device.
	status = sporasub_sp2_send (device, cmd, command, csize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer of the device.
	status = sporasub_sp2_receive (device, cmd + 1, answer, asize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sporasub_sp2_transfer (sporasub_sp2_device_t *device, unsigned char cmd, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = sporasub_sp2_packet (device, cmd, command, csize, answer, asize)) != DC_STATUS_SUCCESS) {
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

	return rc;
}

dc_status_t
sporasub_sp2_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	sporasub_sp2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (sporasub_sp2_device_t *) dc_device_allocate (context, &sporasub_sp2_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (460800 8N1).
	status = dc_iostream_configure (device->iostream, 460800, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Set the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Read the version packet.
	status = sporasub_sp2_packet(device, CMD_VERSION, NULL, 0, device->version, sizeof(device->version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the version packet.");
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
sporasub_sp2_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	sporasub_sp2_device_t *device = (sporasub_sp2_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sporasub_sp2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	sporasub_sp2_device_t *device = (sporasub_sp2_device_t *) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > SZ_READ)
			len = SZ_READ;

		// Build the raw command.
		unsigned char command[] = {
			(address     ) & 0xFF,
			(address >> 8) & 0xFF,
			len};

		// Send the command and receive the answer.
		status = sporasub_sp2_transfer (device, CMD_READ, command, sizeof(command), data + nbytes, len);
		if (status != DC_STATUS_SUCCESS)
			return status;

		nbytes += len;
		address += len;
		data += len;
	}

	return status;
}

static dc_status_t
sporasub_sp2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	sporasub_sp2_device_t *device = (sporasub_sp2_device_t *) abstract;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (device->version + 1);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_READ);
}

static dc_status_t
sporasub_sp2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	sporasub_sp2_device_t *device = (sporasub_sp2_device_t *) abstract;

	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	status = sporasub_sp2_device_dump (abstract, buffer);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free_buffer;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Get the number of dives.
	unsigned int ndives = array_uint16_le (data + 0x02);

	// Get the profile pointer.
	unsigned int eop = array_uint16_le (data + 0x04);
	if (eop < RB_PROFILE_BEGIN || eop > RB_PROFILE_END) {
		ERROR (abstract->context, "Invalid profile pointer (0x%04x).", eop);
		status = DC_STATUS_DATAFORMAT;
		goto error_free_buffer;
	}

	unsigned short *logbook = (unsigned short *) malloc(ndives * sizeof (unsigned short));
	if (logbook == NULL) {
		ERROR (abstract->context, "Out of memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_buffer;
	}

	// Find all dives.
	unsigned int count = 0;
	unsigned int address = RB_PROFILE_BEGIN;
	while (address + SZ_HEADER <= RB_PROFILE_END && count < ndives) {
		if (address == eop) {
			WARNING (abstract->context, "Reached end of profile pointer.");
			break;
		}

		// Get the dive length.
		unsigned int nsamples = array_uint16_le (data + address);
		unsigned int length = SZ_HEADER + nsamples * SZ_SAMPLE;
		if (address + length > RB_PROFILE_END) {
			WARNING (abstract->context, "Reached end of memory.");
			break;
		}

		// Store the address.
		logbook[count] = address;
		count++;

		// The start of the next dive is always aligned to 32 bytes.
		address += iceil (length, SZ_HEADER);
	}

	// Process the dives in reverse order (newest first).
	for (unsigned int i = 0; i < count; ++i) {
		unsigned int idx = count - 1 - i;
		unsigned int offset = logbook[idx];

		// Get the dive length.
		unsigned int nsamples = array_uint16_le (data + offset);
		unsigned int length = SZ_HEADER + nsamples * SZ_SAMPLE;

		// Check the fingerprint data.
		if (memcmp (data + offset + 2, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		if (callback && !callback (data + offset, length, data + offset + 2, sizeof (device->fingerprint), userdata)) {
			break;
		}
	}

	free (logbook);
error_free_buffer:
	dc_buffer_free (buffer);
error_exit:
	return status;
}


static dc_status_t
sporasub_sp2_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	sporasub_sp2_device_t *device = (sporasub_sp2_device_t *) abstract;

	if (datetime->year < 2000) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Build the raw command.
	unsigned char command[] = {
		datetime->year - 2000,
		datetime->month,
		datetime->day,
		datetime->hour,
		datetime->minute,
		datetime->second};

	// Send the command and receive the answer.
	unsigned char answer[1] = {0};
	status = sporasub_sp2_transfer (device, CMD_TIMESYNC, command, sizeof(command), answer, sizeof(answer));
	if (status != DC_STATUS_SUCCESS)
		return status;

	// Verify the response code.
	if (answer[0] != 0) {
		ERROR (abstract->context, "Invalid response code 0x%02x returned.", answer[0]);
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}
