/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
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

#include "tecdiving_divecomputereu.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &tecdiving_divecomputereu_device_vtable)

#define MAXRETRIES  14

#define STX         0x7E

#define CMD_INIT    0x53
#define CMD_LIST    0x57
#define CMD_DIVE    0x58
#define CMD_EXIT    0x59

#define RSP_INIT    0x56
#define RSP_LIST    CMD_LIST
#define RSP_HEADER  0x51
#define RSP_PROFILE 0x52

#define SZ_MAXCMD   2
#define SZ_SUMMARY  7
#define SZ_SAMPLE   8
#define SZ_INIT     56
#define SZ_LIST     (2 + 0x10000 * SZ_SUMMARY)
#define SZ_HEADER   100
#define SZ_PROFILE  (1000 * SZ_SAMPLE)

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct tecdiving_divecomputereu_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[SZ_SUMMARY];
	unsigned char version[SZ_INIT];
} tecdiving_divecomputereu_device_t;

static dc_status_t tecdiving_divecomputereu_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t tecdiving_divecomputereu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t tecdiving_divecomputereu_device_close (dc_device_t *abstract);

static const dc_device_vtable_t tecdiving_divecomputereu_device_vtable = {
	sizeof(tecdiving_divecomputereu_device_t),
	DC_FAMILY_TECDIVING_DIVECOMPUTEREU,
	tecdiving_divecomputereu_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	tecdiving_divecomputereu_device_foreach, /* foreach */
	NULL, /* timesync */
	tecdiving_divecomputereu_device_close, /* close */
};

static unsigned short
checksum_crc (const unsigned char data[], unsigned int size, unsigned short init)
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
tecdiving_divecomputereu_send (tecdiving_divecomputereu_device_t *device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned short crc = 0;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (size > SZ_MAXCMD)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	unsigned char packet[SZ_MAXCMD + 11] = {
		STX,
		0x00,
		(size >>  0) & 0xFF,
		(size >>  8) & 0xFF,
		(size >> 16) & 0xFF,
		(size >> 24) & 0xFF,
		cmd,
	};
	if (size) {
		memcpy(packet + 7, data, size);
	}
	crc = checksum_crc (packet + 1, size + 6, 0);
	packet[size +  7] = (crc >> 8) & 0xFF;
	packet[size +  8] = (crc     ) & 0xFF;
	packet[size +  9] = 0x00;
	packet[size + 10] = 0x00;

	// Give the dive computer some extra time.
	dc_iostream_sleep (device->iostream, 300);

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, size + 11, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
tecdiving_divecomputereu_receive (tecdiving_divecomputereu_device_t *device, unsigned char rsp, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
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
		status = dc_iostream_read (device->iostream, header + 0, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			if (status != DC_STATUS_TIMEOUT) {
				ERROR (abstract->context, "Failed to receive the packet start byte.");
				return status;
			}

			// Abort if the maximum number of retries is reached.
			if (nretries++ >= MAXRETRIES)
				return status;

			// Cancel if requested by the user.
			if (device_is_cancelled (abstract))
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
	status = dc_iostream_read (device->iostream, header + 1, sizeof(header) - 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet header.");
		return status;
	}

	// Verify the type byte.
	unsigned int type = header[1];
	if (type != 0x00) {
		ERROR (abstract->context, "Unexpected type byte (%02x).", type);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the length.
	unsigned int length = array_uint32_le (header + 2);
	if (length > size) {
		ERROR (abstract->context, "Unexpected packet length (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Get the command type.
	unsigned int cmd = header[6];
	if (cmd != rsp) {
		ERROR (abstract->context, "Unexpected command byte (%02x).", cmd);
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
		status = dc_iostream_read (device->iostream, data + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet payload.");
			return status;
		}

		nbytes += len;
	}

	// Read the packet checksum.
	unsigned char checksum[4];
	status = dc_iostream_read (device->iostream, checksum, sizeof(checksum), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet checksum.");
		return status;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be (checksum);
	unsigned short ccrc = 0;
	ccrc = checksum_crc (header + 1, sizeof(header) - 1, ccrc);
	ccrc = checksum_crc (data, length, ccrc);
	if (crc != ccrc || checksum[2] != 0x00 || checksum[3] != 0) {
		ERROR (abstract->context, "Unexpected packet checksum.");
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
tecdiving_divecomputereu_readdive (dc_device_t *abstract, dc_event_progress_t *progress, unsigned int idx, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	tecdiving_divecomputereu_device_t *device = (tecdiving_divecomputereu_device_t *) abstract;

	// Erase the buffer.
	dc_buffer_clear (buffer);

	// Encode the one based logbook ID.
	unsigned int number = idx + 1;
	unsigned char id[] = {
		(number >> 8) & 0xFF,
		(number     ) & 0xFF,
	};

	// Request the dive.
	status = tecdiving_divecomputereu_send (device, CMD_DIVE, id, sizeof(id));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the dive command.");
		return status;
	}

	// Read the dive header.
	unsigned char header[SZ_HEADER];
	status = tecdiving_divecomputereu_receive (device, RSP_HEADER, header, sizeof(header), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the dive header.");
		return status;
	}

	// Get the number of samples.
	unsigned int nsamples = array_uint32_be (header + 36);

	// Calculate the total size.
	unsigned int size = sizeof(header) + nsamples * SZ_SAMPLE;

	// Update and emit a progress event.
	if (progress) {
		progress->current = (idx + 1) * NSTEPS + STEP(sizeof(header), size);
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
		// Get the packet size. The maximum size for a single data
		// packet is 1000 samples.
		unsigned int len = size - nbytes;
		if (len > SZ_PROFILE)
			len = SZ_PROFILE;

		// Read the dive samples.
		status = tecdiving_divecomputereu_receive (device, RSP_PROFILE, data + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the dive samples.");
			return status;
		}

		nbytes += len;

		// Update and emit a progress event.
		if (progress) {
			progress->current = (idx + 1) * NSTEPS + STEP(nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
tecdiving_divecomputereu_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	tecdiving_divecomputereu_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (tecdiving_divecomputereu_device_t *) dc_device_allocate (context, &tecdiving_divecomputereu_device_vtable);
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

	// Send the init command.
	status = tecdiving_divecomputereu_send (device, CMD_INIT, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the init command.");
		goto error_free;
	}

	// Read the device info.
	status = tecdiving_divecomputereu_receive (device, RSP_INIT, device->version, sizeof(device->version), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to receive the device info.");
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
tecdiving_divecomputereu_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	tecdiving_divecomputereu_device_t *device = (tecdiving_divecomputereu_device_t *) abstract;

	status = tecdiving_divecomputereu_send (device, CMD_EXIT, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the exit command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
tecdiving_divecomputereu_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	tecdiving_divecomputereu_device_t *device = (tecdiving_divecomputereu_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
tecdiving_divecomputereu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	tecdiving_divecomputereu_device_t *device = (tecdiving_divecomputereu_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (device->version + 0x22) << 16 | array_uint16_be (device->version + 0x26);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof(device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Allocate memory for the dive list.
	size_t length = SZ_LIST;
	unsigned char *logbook = (unsigned char *) malloc (length);
	if (logbook == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Request the dive list.
	status = tecdiving_divecomputereu_send (device, CMD_LIST, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the list command.");
		goto error_logbook_free;
	}

	// Read the dive list.
	status = tecdiving_divecomputereu_receive (device, RSP_LIST, logbook, length, &length);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the logbook.");
		goto error_logbook_free;
	}

	// Verify the minimum length.
	if (length < 2) {
		status = DC_STATUS_DATAFORMAT;
		goto error_logbook_free;
	}

	// Get the number of logbook entries.
	unsigned int nlogbooks = array_uint16_be (logbook);
	if (length != 2 + nlogbooks * SZ_SUMMARY) {
		status = DC_STATUS_DATAFORMAT;
		goto error_logbook_free;
	}

	// Count the number of dives to download.
	unsigned int ndives = 0;
	for (unsigned int i = 0; i < nlogbooks; ++i) {
		unsigned int offset = 2 + i * SZ_SUMMARY;

		if (memcmp(logbook + offset, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		ndives++;
	}

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (ndives + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate a memory buffer for a single dive.
	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_logbook_free;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int offset = 2 + i * SZ_SUMMARY;

		// Read the dive.
		status = tecdiving_divecomputereu_readdive (abstract, &progress, i, buffer);
		if (status != DC_STATUS_SUCCESS) {
			goto error_buffer_free;
		}

		// Cache the pointer.
		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int size = dc_buffer_get_size(buffer);

		// Verify the logbook entry.
		if (memcmp (data, logbook + offset, SZ_SUMMARY) != 0) {
			ERROR (abstract->context, "Dive header doesn't match logbook entry.");
			status = DC_STATUS_DATAFORMAT;
			goto error_buffer_free;
		}

		if (callback && !callback (data, size, data, sizeof(device->fingerprint), userdata)) {
			break;
		}
	}

error_buffer_free:
	dc_buffer_free (buffer);
error_logbook_free:
	free (logbook);
error_exit:
	return status;
}
