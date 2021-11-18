/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Ryan Gardner, Jef Driesen
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "deepsix_excursion.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

#define MAXPACKET 255

#define HEADERSIZE 156

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

#define FP_SIZE   6
#define FP_OFFSET 12

#define DIR_WRITE          0x00
#define DIR_READ           0x01

#define GRP_INFO           0xA0
#define CMD_INFO_HARDWARE  0x01
#define CMD_INFO_SOFTWARE  0x02
#define CMD_INFO_SERIAL    0x03
#define CMD_INFO_LASTDIVE  0x04

#define GRP_SETTINGS       0xB0
#define CMD_SETTINGS_DATE  0x01
#define CMD_SETTINGS_TIME  0x03
#define CMD_SETTINGS_STORE 0x27
#define CMD_SETTINGS_LOAD  0x28

#define GRP_DIVE           0xC0
#define CMD_DIVE_HEADER    0x02
#define CMD_DIVE_PROFILE   0x03

typedef struct deepsix_excursion_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FP_SIZE];
} deepsix_excursion_device_t;

static dc_status_t deepsix_excursion_device_set_fingerprint (dc_device_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t deepsix_excursion_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepsix_excursion_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t deepsix_excursion_device_vtable = {
	sizeof(deepsix_excursion_device_t),
	DC_FAMILY_DEEPSIX_EXCURSION,
	deepsix_excursion_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	deepsix_excursion_device_foreach, /* foreach */
	deepsix_excursion_device_timesync, /* timesync */
	NULL, /* close */
};

static dc_status_t
deepsix_excursion_send (deepsix_excursion_device_t *device, unsigned char grp, unsigned char cmd, unsigned char dir, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[4 + MAXPACKET + 1];

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (size > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	packet[0] = grp;
	packet[1] = cmd;
	packet[2] = dir;
	packet[3] = size;
	if (size) {
		memcpy(packet + 4, data, size);
	}
	packet[size + 4] = checksum_add_uint8 (packet, size + 4, 0) ^ 0xFF;

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, 4 + size + 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return status;
}

static dc_status_t
deepsix_excursion_recv (deepsix_excursion_device_t *device, unsigned char grp, unsigned char cmd, unsigned char dir, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[4 + MAXPACKET + 1];
	size_t transferred = 0;

	// Read the packet header, payload and checksum.
	status = dc_iostream_read (device->iostream, packet, sizeof(packet), &transferred);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet.");
		return status;
	}

	if (transferred < 4) {
		ERROR (abstract->context, "Packet header too short ("DC_PRINTF_SIZE").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the packet header.
	if (packet[0] != grp || packet[1] != cmd || packet[2] != dir) {
		ERROR (device->base.context, "Unexpected packet header.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int len = packet[3];
	if (len > MAXPACKET) {
		ERROR (abstract->context, "Packet header length too large (%u).", len);
		return DC_STATUS_PROTOCOL;
	}

	if (transferred < 4 + len + 1) {
		ERROR (abstract->context, "Packet data too short ("DC_PRINTF_SIZE").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum.
	unsigned char csum = checksum_add_uint8 (packet, len + 4, 0) ^ 0xFF;
	if (packet[len + 4] != csum) {
		ERROR (abstract->context, "Unexpected packet checksum (%02x)", csum);
		return DC_STATUS_PROTOCOL;
	}

	if (len > size) {
		ERROR (abstract->context, "Unexpected packet length (%u).", len);
		return DC_STATUS_PROTOCOL;
	}

	memcpy(data, packet + 4, len);

	if (actual)
		*actual = len;

	return status;
}

static dc_status_t
deepsix_excursion_transfer (deepsix_excursion_device_t *device, unsigned char grp, unsigned char cmd, unsigned char dir, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	status = deepsix_excursion_send (device, grp, cmd, dir, command, csize);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = deepsix_excursion_recv (device, grp + 1, cmd, dir, answer, asize, actual);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return status;
}

dc_status_t
deepsix_excursion_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_excursion_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (deepsix_excursion_device_t *) dc_device_allocate (context, &deepsix_excursion_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

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
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
deepsix_excursion_device_set_fingerprint (dc_device_t *abstract, const unsigned char *data, unsigned int size)
{
	deepsix_excursion_device_t *device = (deepsix_excursion_device_t *)abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_excursion_device_t *device = (deepsix_excursion_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Load the settings into memory.
	status = deepsix_excursion_transfer (device, GRP_SETTINGS, CMD_SETTINGS_LOAD, DIR_WRITE, NULL, 0, NULL, 0, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to load the settings.");
		return status;
	}

	// Read the hardware version.
	unsigned char rsp_hardware[6] = {0};
	status = deepsix_excursion_transfer (device, GRP_INFO, CMD_INFO_HARDWARE, DIR_READ, NULL, 0, rsp_hardware, sizeof(rsp_hardware), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the hardware version.");
		return status;
	}

	// Read the software version.
	unsigned char rsp_software[6] = {0};
	status = deepsix_excursion_transfer (device, GRP_INFO, CMD_INFO_SOFTWARE, DIR_READ, NULL, 0, rsp_software, sizeof(rsp_software), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the software version.");
		return status;
	}

	// Read the serial number
	unsigned char rsp_serial[12] = {0};
	status = deepsix_excursion_transfer (device, GRP_INFO, CMD_INFO_SERIAL, DIR_READ, NULL, 0, rsp_serial, sizeof(rsp_serial), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the serial number.");
		return status;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint16_be (rsp_software + 4);
	devinfo.serial = array_convert_str2num (rsp_serial + 3, sizeof(rsp_serial) - 3);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Read the index of the last dive.
	const unsigned char cmd_index[2] = {0};
	unsigned char rsp_index[2] = {0};
	status = deepsix_excursion_transfer (device, GRP_INFO, CMD_INFO_LASTDIVE, DIR_READ, cmd_index, sizeof(cmd_index), rsp_index, sizeof(rsp_index), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the last dive index.");
		return status;
	}

	// Calculate the number of dives.
	unsigned int ndives = array_uint16_le (rsp_index);

	// Update and emit a progress event.
	progress.maximum = ndives * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int number = ndives - i;

		const unsigned char cmd_header[] = {
			(number     ) & 0xFF,
			(number >> 8) & 0xFF};
		unsigned char rsp_header[HEADERSIZE] = {0};
		status = deepsix_excursion_transfer (device, GRP_DIVE, CMD_DIVE_HEADER, DIR_READ, cmd_header, sizeof(cmd_header), rsp_header, sizeof(rsp_header), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			goto error_free;
		}

		if (memcmp(rsp_header + FP_OFFSET, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		unsigned int length = array_uint32_le (rsp_header + 8);

		// Update and emit a progress event.
		progress.current = i * NSTEPS + STEP(sizeof(rsp_header), sizeof(rsp_header) + length);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		dc_buffer_clear(buffer);
		dc_buffer_reserve(buffer, sizeof(rsp_header) + length);

		if (!dc_buffer_append(buffer, rsp_header, sizeof(rsp_header))) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			status = DC_STATUS_NOMEMORY;
			goto error_free;
		}

		unsigned offset = 0;
		while (offset < length) {
			unsigned int len = 0;
			const unsigned char cmd_profile[] = {
				(number      ) & 0xFF,
				(number >>  8) & 0xFF,
				(offset      ) & 0xFF,
				(offset >>  8) & 0xFF,
				(offset >> 16) & 0xFF,
				(offset >> 24) & 0xFF};
			unsigned char rsp_profile[MAXPACKET] = {0};
			status = deepsix_excursion_transfer (device, GRP_DIVE, CMD_DIVE_PROFILE, DIR_READ, cmd_profile, sizeof(cmd_profile), rsp_profile, sizeof(rsp_profile), &len);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to read the dive profile.");
				goto error_free;
			}

			// Remove padding from the last packet.
			unsigned int n = len;
			if (offset + n > length) {
				n = length - offset;
			}

			// Update and emit a progress event.
			progress.current = i * NSTEPS + STEP(sizeof(rsp_header) + offset + n, sizeof(rsp_header) + length);
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			if (!dc_buffer_append(buffer, rsp_profile, n)) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				status = DC_STATUS_NOMEMORY;
				goto error_free;
			}

			offset += n;
		}

		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int   size = dc_buffer_get_size(buffer);
		if (callback && !callback (data, size, data + FP_OFFSET, sizeof(device->fingerprint), userdata)) {
			break;
		}
	}

error_free:
	dc_buffer_free(buffer);
	return status;
}

static dc_status_t
deepsix_excursion_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepsix_excursion_device_t *device = (deepsix_excursion_device_t *) abstract;

	if (datetime == NULL || datetime->year < 2000) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	const unsigned char cmd_date[] = {
		datetime->year - 2000,
		datetime->month,
		datetime->day};

	const unsigned char cmd_time[] = {
		datetime->hour,
		datetime->minute,
		datetime->second};

	const unsigned char cmd_store[] = {0x00};

	unsigned char rsp_date[sizeof(cmd_date)] = {0};
	status = deepsix_excursion_transfer (device, GRP_SETTINGS, CMD_SETTINGS_DATE, DIR_WRITE, cmd_date, sizeof(cmd_date), rsp_date, sizeof(rsp_date), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the date.");
		return status;
	}

	if (memcmp(rsp_date, cmd_date, sizeof(cmd_date)) != 0) {
		ERROR (abstract->context, "Failed to verify the date.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned char rsp_time[sizeof(cmd_time)] = {0};
	status = deepsix_excursion_transfer (device, GRP_SETTINGS, CMD_SETTINGS_TIME, DIR_WRITE, cmd_time, sizeof(cmd_time), rsp_time, sizeof(rsp_time), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the time.");
		return status;
	}

	if (memcmp(rsp_time, cmd_time, sizeof(cmd_time)) != 0) {
		ERROR (abstract->context, "Failed to verify the time.");
		return DC_STATUS_PROTOCOL;
	}

	status = deepsix_excursion_transfer (device, GRP_SETTINGS, CMD_SETTINGS_STORE, DIR_WRITE, cmd_store, sizeof(cmd_store), NULL, 0, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to store the settings.");
		return status;
	}

	return status;
}
