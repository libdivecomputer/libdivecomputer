/*
 * libdivecomputer
 *
 * Copyright (C) 2019 Linus Torvalds
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

#include <string.h>
#include <stdlib.h>

#include "deepblu_cosmiq.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

// Maximum data in a packet. It's actually much
// less than this, since BLE packets are small and
// with the 7 bytes of headers and final newline
// and the HEX encoding, the actual maximum is
// just something like 6 bytes.
//
// But in theory the data could be done over
// multiple packets. That doesn't seem to be
// the case in anything I've seen so far.
//
// Pick something small and easy to use for
// stack buffers.
#define MAX_DATA 20

#define SZ_HEADER 36

#define FP_OFFSET   6
#define FP_SIZE     6

#define CMD_SET_DATETIME      0x20

#define CMD_DIVE_COUNT        0x40
#define CMD_DIVE_HEADER       0x41
#define CMD_DIVE_HEADER_DATA  0x42
#define CMD_DIVE_PROFILE      0x43
#define CMD_DIVE_PROFILE_DATA 0x44

#define CMD_SYSTEM_FW         0x58
#define CMD_SYSTEM_MAC        0x5A

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct deepblu_cosmiq_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FP_SIZE];
} deepblu_cosmiq_device_t;

static dc_status_t deepblu_cosmiq_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t deepblu_cosmiq_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepblu_cosmiq_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t deepblu_cosmiq_device_vtable = {
	sizeof(deepblu_cosmiq_device_t),
	DC_FAMILY_DEEPBLU_COSMIQ,
	deepblu_cosmiq_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	deepblu_cosmiq_device_foreach, /* foreach */
	deepblu_cosmiq_device_timesync, /* timesync */
	NULL, /* close */
};

//
// Send a cmd packet.
//
// The format of the cmd on the "wire" is:
//  - byte '#'
//  - HEX char of cmd
//  - HEX char two's complement modular sum of packet data (including cmd/size)
//  - HEX char size of data as encoded in HEX
//  - n * HEX char data
//  - byte '\n'
// so you end up having 8 bytes of header/trailer overhead, and two bytes
// for every byte of data sent due to the HEX encoding.
//
static dc_status_t
deepblu_cosmiq_send (deepblu_cosmiq_device_t *device, const unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > MAX_DATA)
		return DC_STATUS_INVALIDARGS;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Build the packet.
	unsigned char csum = ~checksum_add_uint8 (data, size, cmd + 2 * size) + 1;
	unsigned char raw[3 + MAX_DATA] = {
		cmd,
		csum,
		2 * size
	};
	if (size) {
		memcpy (raw + 3, data, size);
	}
	unsigned char packet[1 + 2 * (3 + MAX_DATA) + 1] = {0};
	packet[0] = '#';
	array_convert_bin2hex (raw, 3 + size, packet + 1, 2 * (3 + size));
	packet[1 + 2 * (3 + size)] = '\n';

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "cmd", raw, 3 + size);

	// Send the command.
	status = dc_iostream_write (device->iostream, packet, 2 + 2 * (3 + size), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the command.");
		return status;
	}

	return status;
}

//
// Receive one 'line' of data
//
// The deepblu BLE protocol is ASCII line based and packetized.
// Normally one packet is one line, but it looks like the Nordic
// Semi BLE chip will sometimes send packets early (some internal
// serial buffer timeout?) with incompete data.
//
// So read packets until you get newline.
static dc_status_t
deepblu_cosmiq_recv_line (deepblu_cosmiq_device_t *device, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t nbytes = 0;

	while (1) {
		unsigned char buffer[20] = {0};
		size_t transferred = 0;

		status = dc_iostream_read (device->iostream, buffer, sizeof(buffer), &transferred);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (device->base.context, "Failed to receive the reply packet.");
			return status;
		}

		if (transferred < 1) {
			ERROR (device->base.context, "Empty reply packet received.");
			return DC_STATUS_PROTOCOL;
		}

		// Append the payload data to the output buffer. If the output
		// buffer is too small, the error is not reported immediately
		// but delayed until all packets have been received.
		if (nbytes < size) {
			size_t n = transferred;
			if (nbytes + n > size) {
				n = size - nbytes;
			}
			memcpy(data + nbytes, buffer, n);
		}
		nbytes += transferred;

		// Last packet?
		if (buffer[transferred - 1] == '\n')
			break;
	}

	// Verify the expected number of bytes.
	if (nbytes > size) {
		ERROR (device->base.context, "Unexpected number of bytes received (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", nbytes, size);
		return DC_STATUS_PROTOCOL;
	}

	if (actual)
		*actual = nbytes;

	return DC_STATUS_SUCCESS;
}

//
// Receive a reply packet
//
// The reply packet has the same format as the cmd packet we
// send, except the first byte is '$' instead of '#'.
static dc_status_t
deepblu_cosmiq_recv (deepblu_cosmiq_device_t *device, const unsigned char cmd, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char packet[1 + 2 * (3 + MAX_DATA) + 1] = {0};

	size_t transferred = 0;
	status = deepblu_cosmiq_recv_line (device, packet, sizeof(packet), &transferred);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred < 8 || (transferred % 2) != 0) {
		ERROR (device->base.context, "Unexpected packet length (" DC_PRINTF_SIZE ").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	if (packet[0] != '$' || packet[transferred - 1] != '\n') {
		ERROR (device->base.context, "Unexpected packet start/end byte.");
		return DC_STATUS_PROTOCOL;
	}

	size_t length = transferred - 2;

	unsigned char raw[3 + MAX_DATA] = {0};
	if (array_convert_hex2bin (packet + 1, length, raw, length / 2) != 0) {
		ERROR (device->base.context, "Unexpected packet data.");
		return DC_STATUS_PROTOCOL;
	}

	length /= 2;

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "rcv", raw, length);

	unsigned char rsp = raw[0];
	if (rsp != cmd) {
		ERROR (device->base.context, "Unexpected packet command byte (%02x)", rsp);
		return DC_STATUS_PROTOCOL;
	}

	unsigned int n = raw[2];
	if ((n % 2) != 0 || n != transferred - 8) {
		ERROR (device->base.context, "Unexpected packet length (%u)", n);
		return DC_STATUS_PROTOCOL;
	}

	unsigned char csum = checksum_add_uint8 (raw, length, 0);
	if (csum != 0) {
		ERROR (device->base.context, "Unexpected packet checksum (%02x).", csum);
		return DC_STATUS_PROTOCOL;
	}

	length -= 3;

	if (length > size) {
		ERROR (device->base.context, "Unexpected number of bytes received (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", length, size);
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, raw + 3, length);

	if (actual)
		*actual = length;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_transfer (deepblu_cosmiq_device_t *device, const unsigned char cmd,
	const unsigned char input[], size_t isize,
	unsigned char output[], size_t osize)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	status = deepblu_cosmiq_send (device, cmd, input, isize);
	if (status != DC_STATUS_SUCCESS)
		return status;

	size_t transferred = 0;
	status = deepblu_cosmiq_recv (device, cmd, output, osize, &transferred);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred != osize) {
		ERROR (device->base.context, "Unexpected number of bytes received (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", osize, transferred);
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_recv_bulk (deepblu_cosmiq_device_t *device, dc_event_progress_t *progress, const unsigned char cmd, unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	const unsigned int initial = progress ? progress->current : 0;

	size_t nbytes = 0;
	while (nbytes < size) {
		size_t transferred = 0;
		status = deepblu_cosmiq_recv (device, cmd, data + nbytes, size - nbytes, &transferred);
		if (status != DC_STATUS_SUCCESS)
			return status;

		nbytes += transferred;

		// Update and emit a progress event.
		if (progress) {
			progress->current = initial + STEP(nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
deepblu_cosmiq_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepblu_cosmiq_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (deepblu_cosmiq_device_t *) dc_device_allocate (context, &deepblu_cosmiq_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof(device->fingerprint));

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
deepblu_cosmiq_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	deepblu_cosmiq_device_t *device = (deepblu_cosmiq_device_t *)abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_cosmiq_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepblu_cosmiq_device_t *device = (deepblu_cosmiq_device_t *) abstract;
	const unsigned char zero = 0;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the firmware version.
	unsigned char fw[1] = {0};
	status = deepblu_cosmiq_transfer (device, CMD_SYSTEM_FW, &zero, 1, fw, sizeof(fw));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the firmware version.");
		goto error_exit;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Firmware", fw, sizeof(fw));

	// Read the MAC address.
	unsigned char mac[6] = {0};
	status = deepblu_cosmiq_transfer (device, CMD_SYSTEM_MAC, &zero, 1, mac, sizeof(mac));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the MAC address.");
		goto error_exit;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Serial", mac, sizeof(mac));

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = fw[0] & 0x3F;
	devinfo.serial = array_uint32_le (mac);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	unsigned char ndives = 0;
	status = deepblu_cosmiq_transfer (device, CMD_DIVE_COUNT, &zero, 1, &ndives, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the number of dives.");
		goto error_exit;
	}

	// Update and emit a progress event.
	progress.current = (ndives == 0 ? 1 : 0) * NSTEPS;
	progress.maximum = (ndives + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	if (ndives == 0) {
		status = DC_STATUS_SUCCESS;
		goto error_exit;
	}

	unsigned char *headers = malloc (ndives * SZ_HEADER);
	if (headers == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	unsigned int count = 0;
	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned char number = i + 1;

		unsigned char len = 0;
		status = deepblu_cosmiq_transfer (device, CMD_DIVE_HEADER, &number, 1, &len, 1);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			goto error_free_headers;
		}

		if (len != SZ_HEADER) {
			status = DC_STATUS_PROTOCOL;
			goto error_free_headers;
		}

		status = deepblu_cosmiq_recv_bulk (device, NULL, CMD_DIVE_HEADER_DATA, headers + i * SZ_HEADER, SZ_HEADER);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			goto error_free_headers;
		}

		// Update and emit a progress event.
		progress.current = STEP(i + 1, ndives);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		if (memcmp (headers + i * SZ_HEADER + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		count++;
	}

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (count + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the dive.
	dc_buffer_t *dive = dc_buffer_new (4096);
	if (dive == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_headers;
	}

	for (unsigned int i = 0; i < count; ++i) {
		unsigned char number = i + 1;

		unsigned char rsp_len[2] = {0};
		status = deepblu_cosmiq_transfer (device, CMD_DIVE_PROFILE, &number, 1, rsp_len, sizeof(rsp_len));
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive profile.");
			goto error_free_dive;
		}

		unsigned int len = array_uint16_be (rsp_len);

		// Erase the buffer.
		dc_buffer_clear (dive);

		// Allocate memory for the dive.
		if (!dc_buffer_resize (dive, len + SZ_HEADER)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_free_dive;
		}

		// Cache the pointer.
		unsigned char *data = dc_buffer_get_data (dive);

		memcpy (data, headers + i * SZ_HEADER, SZ_HEADER);

		status = deepblu_cosmiq_recv_bulk (device, &progress, CMD_DIVE_PROFILE_DATA, data + SZ_HEADER, len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive profile.");
			goto error_free_dive;
		}

		if (callback && !callback (data, len + SZ_HEADER, data + FP_OFFSET, FP_SIZE, userdata))
			break;
	}

error_free_dive:
	dc_buffer_free (dive);
error_free_headers:
	free (headers);
error_exit:
	return status;
}

static dc_status_t
deepblu_cosmiq_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	deepblu_cosmiq_device_t *device = (deepblu_cosmiq_device_t *) abstract;

	if (datetime->year < 2000) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	const unsigned char cmd[6] = {
		dec2bcd(datetime->year - 2000),
		dec2bcd(datetime->month),
		dec2bcd(datetime->day),
		dec2bcd(datetime->hour),
		dec2bcd(datetime->minute),
		dec2bcd(datetime->second)};
	unsigned char rsp[1] = {0};
	status = deepblu_cosmiq_transfer (device, CMD_SET_DATETIME,
		cmd, sizeof(cmd), rsp, sizeof(rsp));
	if (status != DC_STATUS_SUCCESS)
		return status;

	return DC_STATUS_SUCCESS;
}
