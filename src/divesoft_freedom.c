/*
 * libdivecomputer
 *
 * Copyright (C) 2023 Jan Matoušek, Jef Driesen
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

#include "divesoft_freedom.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"
#include "hdlc.h"

#define MAXDATA 256

#define HEADER_SIGNATURE_V1 0x45766944 // "DivE"
#define HEADER_SIGNATURE_V2 0x45566944 // "DiVE"

#define HEADER_SIZE_V1 32
#define HEADER_SIZE_V2 64

#define RECORD_SIZE 16
#define FINGERPRINT_SIZE 20

#define INVALID     0xFFFFFFFF
#define COMPRESSION 1
#define DIRECTION   1
#define NRECORDS    100

#define DEVICE_CCR_CU      1  // Liberty HW rev. 1.X
#define DEVICE_FREEDOM     2  // Freedom HW rev. 2.X
#define DEVICE_FREEDOM3    5  // Freedom HW rev. 3.X
#define DEVICE_CCR_CU15    10 // Liberty HW rev. 2.X, Bluetooth enabled
#define DEVICE_FREEDOM4    19 // Freedom HW rev. 4.X, Bluetooth enabled

typedef enum message_t {
	MSG_ECHO = 0,
	MSG_RESULT = 1,
	MSG_CONNECT = 2,
	MSG_CONNECTED = 3,
	MSG_VERSION = 4,
	MSG_VERSION_RSP = 5,
	MSG_DIVE_DATA = 64,
	MSG_DIVE_DATA_RSP = 65,
	MSG_DIVE_LIST = 66,
	MSG_DIVE_LIST_V1 = 67,
	MSG_DIVE_LIST_V2 = 71,
} message_t;

typedef struct divesoft_freedom_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FINGERPRINT_SIZE];
	unsigned int seqnum;
} divesoft_freedom_device_t;

static dc_status_t divesoft_freedom_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t divesoft_freedom_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t divesoft_freedom_device_close (dc_device_t *device);

static const dc_device_vtable_t divesoft_freedom_device_vtable = {
	sizeof(divesoft_freedom_device_t),
	DC_FAMILY_DIVESOFT_FREEDOM,
	divesoft_freedom_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	divesoft_freedom_device_foreach, /* foreach */
	NULL, /* timesync */
	divesoft_freedom_device_close, /* close */
};

static dc_status_t
divesoft_freedom_send (divesoft_freedom_device_t *device, message_t message, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	size_t nbytes = 0, count = 0;
	while (1) {
		size_t len = size - nbytes;
		if (len > MAXDATA)
			len = MAXDATA;

		unsigned int islast = nbytes + len == size;

		unsigned char packet[6 + MAXDATA + 2] = {0};
		packet[0] = ((count & 0x0F) << 4) | (device->seqnum & 0x0F);
		packet[1] = 0x80 | (islast << 6);
		array_uint16_le_set (packet + 2, message);
		array_uint16_le_set (packet + 4, len);
		if (len) {
			memcpy (packet + 6, data + nbytes, len);
		}
		unsigned short crc = checksum_crc16r_ccitt (packet, len + 6, 0xFFFF, 0xFFFF);
		array_uint16_le_set (packet + 6 + len, crc);

		HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "cmd", packet, 6 + len + 2);

		status = dc_iostream_write (device->iostream, packet, 6 + len + 2, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the packet.");
			return status;
		}

		nbytes += len;
		count++;

		if (islast)
			break;
	}

	return status;
}

static dc_status_t
divesoft_freedom_recv (divesoft_freedom_device_t *device, dc_event_progress_t *progress, message_t *message, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int msg = INVALID;

	unsigned int count = 0;
	while (1) {
		size_t len = 0;
		unsigned char packet[6 + MAXDATA + 2] = {0};
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), &len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return status;
		}

		HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "rcv", packet, len);

		if (len < 8) {
			ERROR (abstract->context, "Unexpected packet length (" DC_PRINTF_SIZE ").", len);
			return DC_STATUS_PROTOCOL;
		}

		unsigned int seqnum = packet[0];
		unsigned int flags = packet[1];
		unsigned int type = array_uint16_le (packet + 2);
		unsigned int length = array_uint16_le (packet + 4);

		unsigned int expected = ((count & 0x0F) << 4) | (device->seqnum & 0x0F);
		if (seqnum != expected) {
			ERROR (abstract->context, "Unexpected packet sequence number (%u %u).", seqnum, expected);
			return DC_STATUS_PROTOCOL;
		}

		if ((flags & ~0x40) != 0) {
			ERROR (abstract->context, "Unexpected packet flags (%u).", flags);
			return DC_STATUS_PROTOCOL;
		}

		if (length != len - 8) {
			ERROR (abstract->context, "Unexpected packet length (%u " DC_PRINTF_SIZE ").", length, len - 8);
			return DC_STATUS_PROTOCOL;
		}

		if (msg == INVALID) {
			msg = type;
		} else if (msg != type) {
			ERROR (abstract->context, "Unexpected packet type (%u).", msg);
			return DC_STATUS_PROTOCOL;
		}

		unsigned short crc = array_uint16_le (packet + len - 2);
		unsigned short ccrc = checksum_crc16r_ccitt (packet, len - 2, 0xFFFF, 0xFFFF);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected packet checksum (%04x %04x).", crc, ccrc);
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len - 8;
			// Limit the progress to the maximum size. This could happen if the
			// dive computer sends more data than requested for some reason.
			if (progress->current > progress->maximum) {
				WARNING (abstract->context, "Progress exceeds the maximum size.");
				progress->current = progress->maximum;
			}
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		if (!dc_buffer_append (buffer, packet + 6, len - 8)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		count++;

		if (flags & 0x40)
			break;
	}

	if (message)
		*message = msg;

	return status;
}

static dc_status_t
divesoft_freedom_transfer (divesoft_freedom_device_t *device, dc_event_progress_t *progress, message_t cmd, const unsigned char data[], size_t size, message_t *msg, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	device->seqnum++;

	status = divesoft_freedom_send (device, cmd, data, size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	status = divesoft_freedom_recv (device, progress, msg, buffer);
	if(status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive response.");
		return status;
	}

	return status;
}

static dc_status_t
divesoft_freedom_download (divesoft_freedom_device_t *device, message_t cmd, const unsigned char cdata[], size_t csize, unsigned char rdata[], size_t rsize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	dc_buffer_t *buffer = dc_buffer_new (rsize);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	message_t msg = MSG_ECHO;
	status = divesoft_freedom_transfer (device, NULL, cmd, cdata, csize, &msg, buffer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to transfer the packet.");
		goto error_free;
	}

	if (msg != cmd + 1) {
		ERROR (abstract->context, "Unexpected response message (%u).", msg);
		status = DC_STATUS_PROTOCOL;
		goto error_free;
	}

	size_t length = dc_buffer_get_size (buffer);
	if (length != rsize) {
		ERROR (abstract->context, "Unexpected response length (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", length, rsize);
		status =  DC_STATUS_PROTOCOL;
		goto error_free;
	}

	if (rsize) {
		memcpy (rdata, dc_buffer_get_data (buffer), rsize);
	}

error_free:
	dc_buffer_free (buffer);
error_exit:
	return status;
}

dc_status_t
divesoft_freedom_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesoft_freedom_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (divesoft_freedom_device_t *) dc_device_allocate (context, &divesoft_freedom_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = NULL;
	memset(device->fingerprint, 0, sizeof(device->fingerprint));
	device->seqnum = 0;

	// Setup the HDLC communication.
	status = dc_hdlc_open (&device->iostream, context, iostream, 244, 244);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to create the HDLC stream.");
		goto error_free;
	}

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free_hdlc;
	}

	// Set the timeout for receiving data (3000ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free_hdlc;
	}

	// Initiate the connection with the dive computer.
	const char client[] = "libdivecomputer";
	unsigned char cmd_connect[2 + sizeof(client) - 1] = {0};
	array_uint16_le_set (cmd_connect, COMPRESSION);
	memcpy (cmd_connect + 2, client, sizeof(client) - 1);
	unsigned char rsp_connect[36] = {0};
	status = divesoft_freedom_download (device, MSG_CONNECT, cmd_connect, sizeof(cmd_connect), rsp_connect, sizeof(rsp_connect));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to connect to the device.");
		goto error_free_hdlc;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Connection", rsp_connect, sizeof(rsp_connect));

	DEBUG (context, "Connection: compression=%u, protocol=%u.%u, serial=%.16s",
		array_uint16_le (rsp_connect),
		rsp_connect[2], rsp_connect[3],
		rsp_connect + 4);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free_hdlc:
	dc_iostream_close (device->iostream);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
divesoft_freedom_device_close (dc_device_t *abstract)
{
	divesoft_freedom_device_t *device = (divesoft_freedom_device_t *) abstract;

	return dc_iostream_close (device->iostream);
}

static dc_status_t
divesoft_freedom_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	divesoft_freedom_device_t *device = (divesoft_freedom_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesoft_freedom_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesoft_freedom_device_t *device = (divesoft_freedom_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the device information.
	unsigned char rsp_version[26] = {0};
	status = divesoft_freedom_download (device, MSG_VERSION, NULL, 0, rsp_version, sizeof(rsp_version));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the device information.");
		goto error_exit;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Version", rsp_version, sizeof(rsp_version));

	DEBUG (abstract->context, "Device: model=%u, hw=%u.%u, sw=%u.%u.%u.%u serial=%.16s",
		rsp_version[0],
		rsp_version[1], rsp_version[2],
		rsp_version[3], rsp_version[4], rsp_version[5],
		array_uint32_le (rsp_version + 6),
		rsp_version + 10);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = rsp_version[0];
	devinfo.firmware = array_uint24_be (rsp_version + 3);
	devinfo.serial = array_convert_str2num (rsp_version + 10 + 5, 11);
	device_event_emit(abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate memory for the dive list.
	dc_buffer_t *divelist = dc_buffer_new (0);
	if (divelist == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Allocate memory for the download buffer.
	dc_buffer_t *buffer = dc_buffer_new (NRECORDS * (4 + FINGERPRINT_SIZE + HEADER_SIZE_V2));
	if (buffer == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_free_divelist;
	}

	// Record version and size.
	unsigned int version = 0;
	unsigned int headersize = 0;
	unsigned int recordsize = 0;

	// Download the dive list.
	unsigned int total = 0;
	unsigned int maxsize = 0;
	unsigned int current = INVALID;
	while (1) {
		// Clear the buffer.
		dc_buffer_clear (buffer);

		// Prepare the command.
		unsigned char cmd_list[6] = {0};
		array_uint32_le_set (cmd_list, current);
		cmd_list[4] = DIRECTION;
		cmd_list[5] = NRECORDS;

		// Download the dive list records.
		message_t msg_list = MSG_ECHO;
		status = divesoft_freedom_transfer (device, &progress, MSG_DIVE_LIST, cmd_list, sizeof(cmd_list), &msg_list, buffer);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the dive list.");
			goto error_free_buffer;
		}

		// Check the response message type.
		if (msg_list != MSG_DIVE_LIST_V1 && msg_list != MSG_DIVE_LIST_V2) {
			ERROR (abstract->context, "Unexpected response message (%u).", msg_list);
			status = DC_STATUS_PROTOCOL;
			goto error_free_buffer;
		}

		// Store/check the version.
		if (version == 0) {
			version = msg_list;
			headersize = version == MSG_DIVE_LIST_V1 ?
				HEADER_SIZE_V1 : HEADER_SIZE_V2;
			recordsize = 4 + FINGERPRINT_SIZE + headersize;
		} else if (version != msg_list) {
			ERROR (abstract->context, "Unexpected response message (%u).", msg_list);
			status = DC_STATUS_PROTOCOL;
			goto error_free_buffer;
		}

		const unsigned char *data = dc_buffer_get_data (buffer);
		size_t size = dc_buffer_get_size (buffer);

		// Process the records.
		size_t offset = 0, count = 0;
		while (offset + recordsize <= size) {
			// Get the record data.
			unsigned int handle = array_uint32_le (data + offset);
			const unsigned char *fingerprint = data + offset + 4;
			const unsigned char *header = data + offset + 4 + FINGERPRINT_SIZE;

			// Check the fingerprint data.
			if (memcmp (device->fingerprint, fingerprint, sizeof(device->fingerprint)) == 0) {
				break;
			}

			// Get the length of the dive.
			unsigned int nrecords = version == MSG_DIVE_LIST_V1 ?
				array_uint32_le (header + 16) & 0x3FFFF :
				array_uint32_le (header + 20);
			unsigned int length = headersize + nrecords * RECORD_SIZE;

			// Calculate the total and maximum size.
			if (length > maxsize)
				maxsize = length;
			total += length;

			// Set the handle for the next request.
			current = handle;

			offset += recordsize;
			count++;
		}

		// Append the records to the dive list buffer.
		if (!dc_buffer_append (divelist, data, count * recordsize)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			status = DC_STATUS_NOMEMORY;
			goto error_free_buffer;
		}

		// Stop downloading if there are no more records.
		if (count < NRECORDS)
			break;
	}

	// Update and emit a progress event.
	progress.maximum = progress.current + total;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	// Reserve memory for the largest dive.
	dc_buffer_reserve (buffer, maxsize);

	const unsigned char *data = dc_buffer_get_data (divelist);
	size_t size = dc_buffer_get_size (divelist);

	size_t offset = 0;
	while (offset + recordsize <= size) {
		// Get the record data.
		unsigned int handle = array_uint32_le (data + offset);
		const unsigned char *fingerprint = data + offset + 4;
		const unsigned char *header = data + offset + 4 + FINGERPRINT_SIZE;

		// Get the length of the dive.
		unsigned int nrecords = version == MSG_DIVE_LIST_V1 ?
			array_uint32_le (header + 16) & 0x3FFFF :
			array_uint32_le (header + 20);
		unsigned int length = headersize + nrecords * RECORD_SIZE;

		// Clear the buffer.
		dc_buffer_clear (buffer);

		// Prepare the command.
		unsigned char cmd_dive[12] = {0};
		array_uint32_le_set (cmd_dive + 0, handle);
		array_uint32_le_set (cmd_dive + 4, 0);
		array_uint32_le_set (cmd_dive + 8, length);

		// Download the dive.
		message_t msg_dive = MSG_ECHO;
		status = divesoft_freedom_transfer (device, &progress, MSG_DIVE_DATA, cmd_dive, sizeof(cmd_dive), &msg_dive, buffer);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the dive.");
			goto error_free_buffer;
		}

		// Check the response message type.
		if (msg_dive != MSG_DIVE_DATA_RSP) {
			ERROR (abstract->context, "Unexpected response message (%u).", msg_dive);
			status = DC_STATUS_PROTOCOL;
			goto error_free_buffer;
		}

		// Verify both dive headers are identical.
		if (dc_buffer_get_size (buffer) < headersize ||
			memcmp (header, dc_buffer_get_data (buffer), headersize) != 0) {
			ERROR (abstract->context, "Unexpected profile header.");
			status = DC_STATUS_PROTOCOL;
			goto error_free_buffer;
		}

		if (callback && !callback (dc_buffer_get_data(buffer), dc_buffer_get_size(buffer), fingerprint, sizeof (device->fingerprint), userdata)) {
			break;
		}

		offset += recordsize;
	}

error_free_buffer:
	dc_buffer_free (buffer);
error_free_divelist:
	dc_buffer_free (divelist);
error_exit:
	return status;
}
