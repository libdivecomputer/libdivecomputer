/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Jef Driesen, David Carron
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

#include "mclean_extreme.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "packet.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mclean_extreme_device_vtable)

#define MAXRETRIES          14

#define STX                 0x7E

#define CMD_SERIALNUMBER    0x91
#define CMD_COMPUTER        0xA0
#define CMD_SET_COMPUTER    0xA1
#define CMD_DIVE            0xA3
#define CMD_CLOSE           0xAA
#define CMD_SET_TIME        0xAC
#define CMD_FIRMWARE        0xAD

#define SZ_PACKET           512
#define SZ_FINGERPRINT      4
#define SZ_CFG              0x002D
#define SZ_COMPUTER         (SZ_CFG + 0x6A)
#define SZ_HEADER           (SZ_CFG + 0x31)
#define SZ_SAMPLE           0x0004

#define EPOCH 946684800 // 2000-01-01 00:00:00 UTC

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct mclean_extreme_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[SZ_FINGERPRINT];
} mclean_extreme_device_t;

static dc_status_t mclean_extreme_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mclean_extreme_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t mclean_extreme_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mclean_extreme_device_close(dc_device_t *abstract);

static const dc_device_vtable_t mclean_extreme_device_vtable = {
	sizeof(mclean_extreme_device_t),
	DC_FAMILY_MCLEAN_EXTREME,
	mclean_extreme_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	mclean_extreme_device_foreach, /* foreach */
	mclean_extreme_device_timesync, /* timesync */
	mclean_extreme_device_close, /* close */
};

static unsigned int
hashcode (const unsigned char data[], size_t size)
{
	unsigned int result = 0;

	for (size_t i = 0; i < size; ++i) {
		result *= 31;
		result += data[i];
	}

	return result;
}

static unsigned short
checksum_crc(const unsigned char data[], unsigned int size, unsigned short init)
{
	unsigned short crc = init;
	for (unsigned int i = 0; i < size; ++i) {
		crc ^= data[i] << 8;
		if (crc & 0x8000) {
			crc <<= 1;
			crc ^= 0x1021;
		} else {
			crc <<= 1;
		}
	}

	return crc;
}

static dc_status_t
mclean_extreme_send(mclean_extreme_device_t *device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *)device;
	unsigned short crc = 0;

	if (device_is_cancelled(abstract))
		return DC_STATUS_CANCELLED;

	if (size > SZ_PACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	unsigned char packet[SZ_PACKET + 11] = {
		STX,
		0x00,
		(size >> 0) & 0xFF,
		(size >> 8) & 0xFF,
		(size >> 16) & 0xFF,
		(size >> 24) & 0xFF,
		cmd,
	};
	if (size) {
		memcpy(packet + 7, data, size);
	}
	crc = checksum_crc(packet + 1, size + 6, 0);
	packet[size + 7] = (crc >> 8) & 0xFF;
	packet[size + 8] = (crc) & 0xFF;
	packet[size + 9] = 0x00;
	packet[size + 10] = 0x00;

	// Give the dive computer some extra time.
	dc_iostream_sleep(device->iostream, 300);

	// Send the data packet.
	status = dc_iostream_write(device->iostream, packet, size + 11, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_receive(mclean_extreme_device_t *device, unsigned char rsp, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *)device;
	unsigned char header[7];
	unsigned int nretries = 0;

	// Read the packet start byte.
	// Unfortunately it takes a relative long time, about 6-8 seconds,
	// before the STX byte arrives. Hence the standard timeout of one
	// second is not sufficient, and we need to retry a few times on
	// timeout. The advantage over using a single read operation with a
	// large timeout is that we can give the user a chance to cancel the
	// operation.
	while (1) {
		status = dc_iostream_read(device->iostream, header + 0, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			if (status != DC_STATUS_TIMEOUT) {
				ERROR(abstract->context, "Failed to receive the packet start byte.");
				return status;
			}

			// Abort if the maximum number of retries is reached.
			if (nretries++ >= MAXRETRIES)
				return status;

			// Cancel if requested by the user.
			if (device_is_cancelled(abstract))
				return DC_STATUS_CANCELLED;

			// Try again.
			continue;
		}

		if (header[0] == STX)
			break;

		// Reset the retry counter.
		nretries = 0;
	}

	// Read the packet header.
	status = dc_iostream_read(device->iostream, header + 1, sizeof(header) - 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to receive the packet header.");
		return status;
	}

	// Verify the type byte.
	unsigned int type = header[1];
	if (type != 0x00) {
		ERROR(abstract->context, "Unexpected type byte (%02x).", type);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the length.
	unsigned int length = array_uint32_le(header + 2);
	if (length > size) {
		ERROR(abstract->context, "Unexpected packet length (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Get the command type.
	unsigned int cmd = header[6];
	if (cmd != rsp) {
		ERROR(abstract->context, "Unexpected command byte (%02x).", cmd);
		return DC_STATUS_PROTOCOL;
	}

	size_t nbytes = 0;
	while (nbytes < length) {
		// Set the maximum packet size.
		size_t len = 1000;

		// Limit the packet size to the total size.
		if (nbytes + len > length)
			len = length - nbytes;

		// Read the packet payload.
		status = dc_iostream_read(device->iostream, data + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR(abstract->context, "Failed to receive the packet payload.");
			return status;
		}

		nbytes += len;
	}

	// Read the packet checksum.
	unsigned char checksum[4];
	status = dc_iostream_read(device->iostream, checksum, sizeof(checksum), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to receive the packet checksum.");
		return status;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be(checksum);
	unsigned short ccrc = 0;
	ccrc = checksum_crc(header + 1, sizeof(header) - 1, ccrc);
	ccrc = checksum_crc(data, length, ccrc);
	if (crc != ccrc || checksum[2] != 0x00 || checksum[3] != 0) {
		ERROR(abstract->context, "Unexpected packet checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (actual == NULL) {
		// Verify the actual length.
		if (length != size) {
			ERROR (abstract->context, "Unexpected packet length (%u).", length);
			return DC_STATUS_PROTOCOL;
		}
	} else {
		// Return the actual length.
		*actual = length;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_transfer(mclean_extreme_device_t *device, unsigned char cmd, const unsigned char data[], size_t size, unsigned char answer[], size_t asize, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	// Send the command
	status = mclean_extreme_send(device, cmd, data, size);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Receive the answer
	if (asize) {
		status = mclean_extreme_receive(device, cmd, answer, asize, actual);
		if (status != DC_STATUS_SUCCESS) {
			return status;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_readdive (dc_device_t *abstract, dc_event_progress_t *progress, unsigned int idx, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t *device = (mclean_extreme_device_t *) abstract;

	// Erase the buffer.
	dc_buffer_clear (buffer);

	// Encode the logbook ID.
	unsigned char id[] = {
		(idx     ) & 0xFF,
		(idx >> 8) & 0xFF,
	};

	// Update and emit a progress event.
	unsigned int initial = 0;
	if (progress) {
		initial = progress->current;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	// Request the dive.
	status = mclean_extreme_send (device, CMD_DIVE, id, sizeof(id));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the dive command.");
		return status;
	}

	// Read the dive header.
	unsigned char header[SZ_HEADER];
	status = mclean_extreme_receive (device, CMD_DIVE, header, sizeof(header), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the dive header.");
		return status;
	}

	// Verify the format version.
	unsigned int format = header[0x0000];
	if (format != 0) {
		ERROR(abstract->context, "Unrecognised dive format.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of samples.
	unsigned int nsamples = array_uint16_le (header + 0x005C);

	// Calculate the total size.
	unsigned int size = sizeof(header) + nsamples * SZ_SAMPLE;

	// Update and emit a progress event.
	if (progress) {
		progress->current = initial + STEP(sizeof(header), size);
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	// Allocate memory for the dive.
	if (!dc_buffer_resize (buffer, size)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Cache the pointer.
	unsigned char *data = dc_buffer_get_data(buffer);

	// Append the header.
	memcpy (data, header, sizeof(header));

	unsigned int nbytes = sizeof(header);
	while (nbytes < size) {
		// Get the maximum packet size.
		size_t len = size - nbytes;

		// Read the dive samples.
		status = mclean_extreme_receive (device, CMD_DIVE, data + nbytes, len, &len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the dive samples.");
			return status;
		}

		nbytes += len;

		// Update and emit a progress event.
		if (progress) {
			progress->current = initial + STEP(nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
mclean_extreme_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t *device = NULL;
	dc_transport_t transport = dc_iostream_get_transport (iostream);

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mclean_extreme_device_t *)dc_device_allocate(context, &mclean_extreme_device_vtable);
	if (device == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

	// Create the packet stream.
	if (transport == DC_TRANSPORT_BLE) {
		status = dc_packet_open (&device->iostream, context, iostream, 244, 244);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to create the packet stream.");
			goto error_free;
		}
	} else {
		device->iostream = iostream;
	}

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure(device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to set the terminal attributes.");
		goto error_free_iostream;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout(device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to set the timeout.");
		goto error_free_iostream;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *)device;

	return DC_STATUS_SUCCESS;

error_free_iostream:
	if (transport == DC_TRANSPORT_BLE) {
		dc_iostream_close (device->iostream);
	}
error_free:
	dc_device_deallocate((dc_device_t *)device);
	return status;
}

static dc_status_t
mclean_extreme_device_close(dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t *device = (mclean_extreme_device_t *)abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	rc = mclean_extreme_send(device, CMD_CLOSE, NULL, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to send the exit command.");
		dc_status_set_error(&status, rc);
	}

	// Close the packet stream.
	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE) {
		rc = dc_iostream_close (device->iostream);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to close the packet stream.");
			dc_status_set_error(&status, rc);
		}
	}

	return status;
}

static dc_status_t
mclean_extreme_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mclean_extreme_device_t *device = (mclean_extreme_device_t *)abstract;

	if (size && size != sizeof(device->fingerprint)) {
		return DC_STATUS_INVALIDARGS;
	}

	if (size) {
		memcpy(device->fingerprint, data, sizeof(device->fingerprint));
	} else {
		memset(device->fingerprint, 0, sizeof(device->fingerprint));
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
	mclean_extreme_device_t *device = (mclean_extreme_device_t *)abstract;

	// Get the UTC timestamp.
	dc_ticks_t ticks = dc_datetime_mktime(datetime);
	if (ticks == -1 || ticks < EPOCH || ticks - EPOCH > 0xFFFFFFFF) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Adjust the epoch.
	unsigned int timestamp = ticks - EPOCH;

	// Send the command.
	const unsigned char cmd[] = {
		(timestamp      ) & 0xFF,
		(timestamp >>  8) & 0xFF,
		(timestamp >> 16) & 0xFF,
		(timestamp >> 24) & 0xFF
	};
	dc_status_t status = mclean_extreme_send(device, CMD_SET_TIME, cmd, sizeof(cmd));
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to send the set time command.");
		return status;
	}

	return status;
}

static dc_status_t
mclean_extreme_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t *device = (mclean_extreme_device_t *)abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the firmware version.
	unsigned char firmware[4] = {0};
	status = mclean_extreme_transfer(device, CMD_FIRMWARE, NULL, 0, firmware, sizeof(firmware), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to read the firmware version.");
		return status;
	}

	// Read the serial number.
	size_t serial_len = 0;
	unsigned char serial[SZ_PACKET] = {0};
	status = mclean_extreme_transfer(device, CMD_SERIALNUMBER, NULL, 0, serial, sizeof(serial), &serial_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to read serial number.");
		return status;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint32_le (firmware);
	devinfo.serial = hashcode (serial, serial_len);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Read the computer configuration.
	unsigned char computer[SZ_COMPUTER];
	status = mclean_extreme_transfer(device, CMD_COMPUTER, NULL, 0, computer, sizeof(computer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to read the computer configuration.");
		return status;
	}

	// Verify the format version.
	unsigned int format = computer[0x0000];
	if (format != 0) {
		ERROR(abstract->context, "Unsupported device format.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of dives.
	unsigned int ndives = array_uint16_le(computer + 0x0019);

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (ndives + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate a memory buffer for a single dive.
	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		// Download in reverse order (newest first).
		unsigned int idx = ndives - 1 - i;

		// Read the dive.
		status = mclean_extreme_readdive (abstract, &progress, idx, buffer);
		if (status != DC_STATUS_SUCCESS) {
			goto error_buffer_free;
		}

		// Cache the pointer.
		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int size = dc_buffer_get_size(buffer);

		if (memcmp(data + SZ_CFG, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		if (callback && !callback (data, size, data + SZ_CFG, sizeof(device->fingerprint), userdata)) {
			break;
		}
	}

error_buffer_free:
	dc_buffer_free (buffer);
error_exit:
	return status;
}
