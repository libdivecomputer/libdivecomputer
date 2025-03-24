/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Jef Driesen
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

#include "halcyon_symbios.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

#define CMD_GET_STATUS         0x01
#define CMD_GET_SETTINGS       0x02
#define CMD_SET_SETTINGS       0x03
#define CMD_LOGBOOK_REQUEST    0x04
#define CMD_DIVELOG_REQUEST    0x05
#define CMD_SET_TIME           0x07
#define CMD_LOGBOOK_BLOCK      0x08
#define CMD_DIVELOG_BLOCK      0x09

#define CMD_RESPONSE           0x80

#define ERR_BASE         0x80000000
#define ERR_CRC          0
#define ERR_BOUNDARY     1
#define ERR_CMD_LENGTH   2
#define ERR_CMD_UNKNOWN  3
#define ERR_TIMEOUT      4
#define ERR_FILE         5
#define ERR_UNKNOWN      6

#define ACK 0x06
#define NAK 0x15

#define MAXRETRIES 3

#define MAXPACKET 256

#define SZ_BLOCK   200
#define SZ_LOGBOOK 32

#define FP_OFFSET 20
#define FP_SIZE 4

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct halcyon_symbios_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FP_SIZE];
} halcyon_symbios_device_t;

static dc_status_t halcyon_symbios_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t halcyon_symbios_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t halcyon_symbios_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t halcyon_symbios_device_vtable = {
	sizeof(halcyon_symbios_device_t),
	DC_FAMILY_HALCYON_SYMBIOS,
	halcyon_symbios_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	halcyon_symbios_device_foreach, /* foreach */
	halcyon_symbios_device_timesync, /* timesync */
	NULL, /* close */
};

static dc_status_t
halcyon_symbios_send (halcyon_symbios_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[1 + MAXPACKET + 1] = {0};
	unsigned int length = 1;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (size > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	packet[0] = cmd;
	if (size) {
		memcpy (packet + 1, data, size);
		packet[1 + size] = checksum_crc8 (data, size, 0x00, 0x00);
		length += size + 1;
	}

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, length, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
halcyon_symbios_recv (halcyon_symbios_device_t *device, unsigned char cmd, unsigned char data[], unsigned int size, unsigned int *actual, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[2 + MAXPACKET + 1] = {0};
	unsigned int length = 0;
	unsigned int errcode = 0;

	// Receive the answer.
	size_t len = 0;
	status = dc_iostream_read (device->iostream, packet, sizeof(packet), &len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet.");
		goto error_exit;
	}

	// Verify the minimum length of the packet.
	if (len < 3) {
		ERROR (abstract->context, "Unexpected packet length (" DC_PRINTF_SIZE ").", len);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	// Verify the checksum.
	unsigned char crc = packet[len - 1];
	unsigned char ccrc = checksum_crc8 (packet + 1, len - 2, 0x00, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected packet checksum (%02x %02x).", crc, ccrc);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	// Verify the command byte.
	unsigned int rsp = packet[0];
	unsigned int expected = cmd | CMD_RESPONSE;
	if (rsp != expected) {
		ERROR (abstract->context, "Unexpected command byte (%02x).", rsp);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	// Verify the ACK/NAK byte.
	unsigned int type = packet[1];
	if (type != ACK && type != NAK) {
		ERROR (abstract->context, "Unexpected ACK/NAK byte (%02x).", type);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	// Get the error code from a NAK packet.
	if (type == NAK) {
		// Verify the length of the NAK packet.
		if (len != 4) {
			ERROR (abstract->context, "Unexpected NAK packet length (" DC_PRINTF_SIZE ").", len);
			status = DC_STATUS_PROTOCOL;
			goto error_exit;
		}

		// Set the ERR_BASE bit to indicate an error code is available.
		errcode = packet[2] | ERR_BASE;

		ERROR (abstract->context, "Received NAK packet with error code %u.", errcode & ~ERR_BASE);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	// Verify the maximum length of the packet.
	if (len - 3 > size) {
		ERROR (abstract->context, "Unexpected packet length (" DC_PRINTF_SIZE ").", len);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	if (len - 3) {
		memcpy (data, packet + 2, len - 3);
	}
	length = len - 3;

error_exit:
	if (actual) {
		*actual = length;
	}
	if (errorcode) {
		*errorcode = errcode;
	}
	return status;
}

static dc_status_t
halcyon_symbios_transfer (halcyon_symbios_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int errcode = 0;

	// Send the command.
	status = halcyon_symbios_send (device, cmd, data, size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		goto error_exit;
	}

	// Receive the answer.
	unsigned int length = 0;
	status = halcyon_symbios_recv (device, cmd, answer, asize, &length, &errcode);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		goto error_exit;
	}

	// Verify the length of the packet.
	if (length != asize) {
		ERROR (abstract->context, "Unexpected packet length (%u).", length);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

error_exit:
	if (errorcode) {
		*errorcode = errcode;
	}
	return status;
}

static dc_status_t
halcyon_symbios_download (halcyon_symbios_device_t *device, dc_event_progress_t *progress, unsigned char request, const unsigned char data[], unsigned int size, unsigned char block, dc_buffer_t *buffer, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int errcode = 0;

	// Clear the buffer.
	dc_buffer_clear (buffer);

	// Request the data.
	unsigned char response[4] = {0};
	status = halcyon_symbios_transfer (device, request, data, size, response, sizeof(response), &errcode);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to request the data.");
		goto error_exit;
	}

	// Get the total length.
	unsigned int length = array_uint32_le (response);

	// Resize the buffer.
	if (!dc_buffer_reserve (buffer, length)) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Send the request for the first data block.
	status = halcyon_symbios_send (device, block, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		goto error_exit;
	}

	const unsigned int initial = progress ? progress->current : 0;

	unsigned int counter = 1;
	unsigned int nbytes = 0;
	while (1) {
		// Receive the data block.
		unsigned int len = 0;
		unsigned char payload[2 + SZ_BLOCK] = {0};
		unsigned int nretries = 0;
		while ((status = halcyon_symbios_recv (device, block, payload, sizeof(payload), &len, NULL)) != DC_STATUS_SUCCESS) {
			// Abort if the error is fatal.
			if (status != DC_STATUS_PROTOCOL) {
				ERROR (abstract->context, "Failed to receive the answer.");
				goto error_exit;
			}

			// Abort if the maximum number of retries is reached.
			if (nretries++ >= MAXRETRIES) {
				ERROR (abstract->context, "Reached the maximum number of retries.");
				goto error_exit;
			}

			// Send a NAK to request a re-transmission.
			status = halcyon_symbios_send (device, NAK, NULL, 0);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to send the NAK.");
				goto error_exit;
			}
		}

		// Verify the minimum block length.
		if (len < 2) {
			ERROR (abstract->context, "Unexpected block length (%u).", len);
			status = DC_STATUS_PROTOCOL;
			goto error_exit;
		}

		// Verify the sequence number.
		unsigned int id = array_uint16_le (payload);
		unsigned int seqnum = id & 0x7FFF;
		if (seqnum != counter) {
			ERROR (abstract->context, "Unexpected block sequence number (%04x %04x).", seqnum, counter);
			status = DC_STATUS_PROTOCOL;
			goto error_exit;
		}

		// Append the payload data to the output buffer.
		if (!dc_buffer_append (buffer, payload + 2, len - 2)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_exit;
		}

		nbytes += len - 2;
		counter += 1;
		counter &= 0x7FFF;

		// Update and emit a progress event.
		if (progress) {
			// Limit the progress events to the announced length.
			unsigned int n = nbytes > length ? length : nbytes;
			progress->current = initial + STEP(n, length);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		// Send an ACK to request the next block or finalize the download.
		status = halcyon_symbios_send (device, ACK, NULL, 0);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the ACK.");
			goto error_exit;
		}

		// Check for the last packet.
		if (id & 0x8000)
			break;
	}

	// Verify the length of the data.
	if (nbytes != length) {
		ERROR (abstract->context, "Unexpected data length (%u %u).", nbytes, length);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

error_exit:
	if (errorcode) {
		*errorcode = errcode;
	}
	return status;
}

dc_status_t
halcyon_symbios_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	halcyon_symbios_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (halcyon_symbios_device_t *) dc_device_allocate (context, &halcyon_symbios_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

	// Set the timeout for receiving data (3000ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
halcyon_symbios_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	halcyon_symbios_device_t *device = (halcyon_symbios_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
halcyon_symbios_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	halcyon_symbios_device_t *device = (halcyon_symbios_device_t *) abstract;
	unsigned int errcode = 0;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the device status.
	unsigned char info[20] = {0};
	status = halcyon_symbios_transfer (device, CMD_GET_STATUS, NULL, 0, info, sizeof(info), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the device status.");
		goto error_exit;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Version", info, sizeof(info));

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = info;
	vendor.size = sizeof(info);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = info[5];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (info);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	DEBUG (abstract->context, "Device: serial=%u, hw=%u, model=%u, bt=%u.%u, battery=%u, pressure=%u, errorbits=%u",
		array_uint32_le (info),
		info[4], info[5], info[6], info[7],
		array_uint16_le (info + 8),
		array_uint16_le (info + 10),
		array_uint32_le (info + 12));

	dc_buffer_t *logbook = dc_buffer_new (0);
	dc_buffer_t *dive = dc_buffer_new (0);
	if (logbook == NULL || dive == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free;
	}

	// Download the logbook.
	status = halcyon_symbios_download (device, &progress, CMD_LOGBOOK_REQUEST, NULL, 0, CMD_LOGBOOK_BLOCK, logbook, &errcode);
	if (status != DC_STATUS_SUCCESS) {
		if (errcode == (ERR_FILE | ERR_BASE)) {
			WARNING (abstract->context, "Logbook not available!");

			// Update and emit a progress event.
			progress.current = NSTEPS;
			progress.maximum = NSTEPS;
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			status = DC_STATUS_SUCCESS;
			goto error_free;
		}
		ERROR (abstract->context, "Failed to download the logbook.");
		goto error_free;
	}

	const unsigned char *data = dc_buffer_get_data (logbook);
	size_t size = dc_buffer_get_size (logbook);

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Logbook", data, size);

	// Get the number of dives.
	unsigned int ndives = 0;
	unsigned int offset = size;
	while (offset >= SZ_LOGBOOK) {
		offset -= SZ_LOGBOOK;

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (data + offset + FP_OFFSET, device->fingerprint, sizeof(device->fingerprint)) == 0) {
			break;
		}

		ndives++;
	}

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (ndives + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	offset = size;
	for (unsigned int i = 0; i < ndives; ++i) {
		offset -= SZ_LOGBOOK;

		// Clear the buffer.
		dc_buffer_clear (dive);

		// Download the dive.
		status = halcyon_symbios_download (device, &progress, CMD_DIVELOG_REQUEST, data + offset + 16, 2, CMD_DIVELOG_BLOCK, dive, &errcode);
		if (status != DC_STATUS_SUCCESS) {
			if (errcode == (ERR_FILE | ERR_BASE)) {
				WARNING (abstract->context, "Dive #%u not available!",
					array_uint16_le (data + offset + 16));

				// Update and emit a progress event.
				progress.current = (i + 2) * NSTEPS;
				device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

				status = DC_STATUS_SUCCESS;
				continue;
			}
			ERROR (abstract->context, "Failed to download the dive.");
			goto error_free;
		}

		if (callback && !callback (dc_buffer_get_data (dive), dc_buffer_get_size (dive), data + offset + FP_OFFSET, FP_SIZE, userdata)) {
			break;
		}
	}

error_free:
	dc_buffer_free (dive);
	dc_buffer_free (logbook);
error_exit:
	return status;
}

static dc_status_t
halcyon_symbios_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	halcyon_symbios_device_t *device = (halcyon_symbios_device_t *) abstract;

	unsigned char request[] = {
		datetime->year - 2000,
		datetime->month,
		datetime->day,
		datetime->hour,
		datetime->minute,
		datetime->second,
	};
	status = halcyon_symbios_transfer (device, CMD_SET_TIME, request, sizeof(request), NULL, 0, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the time.");
		goto error_exit;
	}

error_exit:
	return status;
}
