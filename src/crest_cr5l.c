/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Kalen Josifovski
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

#include <stdlib.h>
#include <string.h>

#include <libdivecomputer/buffer.h>

#include "crest_cr5l.h"
#include "array.h"
#include "context-private.h"
#include "device-private.h"

#define CR5L_FP_SIZE         8
#define CR5L_PACKET_MAX      32
#define CR5L_NAMESPACE       "DIVELOG"
#define CR5L_NAMESPACE_SIZE  7
#define CR5L_ENTRY_SIZE      18
#define CR5L_CHUNK_DATA_SIZE 19
#define CR5L_BLOCK_CHUNKS    256

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct crest_cr5l_entry_t {
	unsigned char index;
	unsigned char dive_id[CR5L_FP_SIZE];
	unsigned int size;
	/* Observed in the list entry trailer. Preserve it for now so future
	 * work can identify it without changing the transfer structure again. */
	unsigned int metadata;
} crest_cr5l_entry_t;

typedef struct crest_cr5l_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[CR5L_FP_SIZE];
	unsigned char pending_packet[CR5L_PACKET_MAX];
	unsigned int pending_size;
	unsigned char pending_available;
} crest_cr5l_device_t;

static dc_status_t crest_cr5l_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t crest_cr5l_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t crest_cr5l_device_vtable = {
	sizeof(crest_cr5l_device_t),
	DC_FAMILY_CREST_CR5L,
	crest_cr5l_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	crest_cr5l_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL, /* close */
};

static dc_status_t
crest_cr5l_send (crest_cr5l_device_t *device, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (device_is_cancelled ((dc_device_t *) device))
		return DC_STATUS_CANCELLED;

	status = dc_iostream_write (device->iostream, data, size, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the packet.");
		return status;
	}

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "snd", data, size);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_recv (crest_cr5l_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t transferred = 0;

	status = dc_iostream_read (device->iostream, data, size, &transferred);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to receive the packet.");
		return status;
	}

	if (transferred == 0) {
		ERROR (device->base.context, "Empty packet received.");
		return DC_STATUS_PROTOCOL;
	}

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "rcv", data, transferred);

	if (actual)
		*actual = transferred;

	return DC_STATUS_SUCCESS;
}

static unsigned int
crest_cr5l_parse_suffix_number (const unsigned char data[], unsigned int size)
{
	unsigned int first = size;
	for (unsigned int i = 0; i < size; ++i) {
		if (data[i] >= '0' && data[i] <= '9') {
			first = i;
			break;
		}
	}

	if (first == size)
		return 0;

	return array_convert_str2num (data + first, size - first);
}

static dc_status_t
crest_cr5l_query_version (crest_cr5l_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	static const unsigned char command[] = {0xA0, 0x01, 0x01, 0x5D};
	dc_status_t status = crest_cr5l_send (device, command, sizeof(command));
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = crest_cr5l_recv (device, data, size, actual);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (*actual < 4 || data[0] != 0xA1 || data[1] != 0x01)
		return DC_STATUS_PROTOCOL;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_query_serial (crest_cr5l_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	static const unsigned char command[] = {0xA0, 0x02, 0x01, 0x5C};
	dc_status_t status = crest_cr5l_send (device, command, sizeof(command));
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = crest_cr5l_recv (device, data, size, actual);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (*actual < 4 || data[0] != 0xA1 || data[1] != 0x02)
		return DC_STATUS_PROTOCOL;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_emit_devinfo (crest_cr5l_device_t *device)
{
	unsigned char version[CR5L_PACKET_MAX] = {0};
	unsigned char serial[CR5L_PACKET_MAX] = {0};
	unsigned int version_size = 0, serial_size = 0;

	dc_status_t status = crest_cr5l_query_version (device, version, sizeof(version), &version_size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to query the firmware version.");
		return status;
	}

	status = crest_cr5l_query_serial (device, serial, sizeof(serial), &serial_size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to query the serial number.");
		return status;
	}

	dc_event_devinfo_t devinfo = {0};
	devinfo.model = 0;
	devinfo.firmware = crest_cr5l_parse_suffix_number (version + 3, version_size > 4 ? version_size - 4 : 0);
	devinfo.serial = crest_cr5l_parse_suffix_number (serial + 3, serial_size > 4 ? serial_size - 4 : 0);
	device_event_emit ((dc_device_t *) device, DC_EVENT_DEVINFO, &devinfo);

	dc_event_vendor_t vendor = {0};
	vendor.data = serial;
	vendor.size = serial_size;
	device_event_emit ((dc_device_t *) device, DC_EVENT_VENDOR, &vendor);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_send_session_init (crest_cr5l_device_t *device)
{
	static const unsigned char command[] = {0x06, 0xC0, 0x01, 0xD9, 0x69};
	dc_status_t status = crest_cr5l_send (device, command, sizeof(command));
	if (status != DC_STATUS_SUCCESS)
		return status;

	/* The official app pauses for a few seconds after this command before
	 * proceeding with the final pre-list queries and DIVELOG enumeration. */
	dc_iostream_sleep (device->iostream, 3500);
	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_list_dives (crest_cr5l_device_t *device, crest_cr5l_entry_t entries[], unsigned int capacity, unsigned int *count)
{
	/* The list request uses the literal DIVELOG namespace from the official
	 * app capture. */
	static const unsigned char command[] = {0x38, 0x00, 'D', 'I', 'V', 'E', 'L', 'O', 'G'};
	unsigned char packet[CR5L_PACKET_MAX] = {0};
	unsigned int transferred = 0;

	dc_status_t status = crest_cr5l_send (device, command, sizeof(command));
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred >= 2 && packet[0] == 0x38) {
		unsigned int ndives = packet[1];
		if (ndives > capacity)
			return DC_STATUS_NOMEMORY;

		for (unsigned int i = 0; i < ndives; ++i) {
			status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
			if (status != DC_STATUS_SUCCESS)
				return status;

			if (transferred != CR5L_ENTRY_SIZE)
				return DC_STATUS_PROTOCOL;

			entries[i].index = packet[0];
			memcpy (entries[i].dive_id, packet + 2, CR5L_FP_SIZE);
			entries[i].size = array_uint32_le (packet + 10);
			entries[i].metadata = array_uint32_le (packet + 14);
		}

		if (count)
			*count = ndives;

		return DC_STATUS_SUCCESS;
	}

	if (transferred != CR5L_ENTRY_SIZE)
		return DC_STATUS_PROTOCOL;

	unsigned int ndives = 0;
	do {
		if (ndives >= capacity)
			return DC_STATUS_NOMEMORY;

		entries[ndives].index = packet[0];
		memcpy (entries[ndives].dive_id, packet + 2, CR5L_FP_SIZE);
		entries[ndives].size = array_uint32_le (packet + 10);
		entries[ndives].metadata = array_uint32_le (packet + 14);
		ndives++;

		status = dc_iostream_set_timeout (device->iostream, 500);
		if (status != DC_STATUS_SUCCESS)
			return status;

		status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
	} while (status == DC_STATUS_SUCCESS && transferred == CR5L_ENTRY_SIZE);

	dc_status_t restore = dc_iostream_set_timeout (device->iostream, 5000);
	if (restore != DC_STATUS_SUCCESS)
		return restore;

	if (status == DC_STATUS_SUCCESS) {
		if (transferred >= 2 && packet[0] == 0x38) {
			unsigned int reported = packet[1];
			if (reported != ndives)
				return DC_STATUS_PROTOCOL;
		} else {
			return DC_STATUS_PROTOCOL;
		}
	} else if (status != DC_STATUS_TIMEOUT) {
		return status;
	}

	if (count)
		*count = ndives;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_open_dive (crest_cr5l_device_t *device, const crest_cr5l_entry_t *entry)
{
	/* 0x33 packets are <opcode, pad, "DIVELOG", NUL, 8-byte dive id>. */
	unsigned char command[2 + CR5L_NAMESPACE_SIZE + 1 + CR5L_FP_SIZE] = {0};
	unsigned int offset = 0;
	command[offset++] = 0x33;
	command[offset++] = 0x00;
	memcpy (command + offset, CR5L_NAMESPACE, CR5L_NAMESPACE_SIZE);
	offset += CR5L_NAMESPACE_SIZE;
	command[offset++] = 0x00;
	memcpy (command + offset, entry->dive_id, CR5L_FP_SIZE);
	offset += CR5L_FP_SIZE;

	dc_status_t status = crest_cr5l_send (device, command, offset);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned char packet[CR5L_PACKET_MAX] = {0};
	unsigned int transferred = 0;
	status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred < offset + 1 || packet[0] != 0x33 || packet[transferred - 1] != 0x01)
		return DC_STATUS_PROTOCOL;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_request_block (crest_cr5l_device_t *device, const crest_cr5l_entry_t *entry, unsigned int block_index, unsigned int chunks)
{
	/* 0x39 packets are a 6-byte block header followed by the DIVELOG
	 * namespace and a trailing NUL. */
	unsigned char command[6 + CR5L_NAMESPACE_SIZE + 1] = {0};
	command[0] = 0x39;
	command[1] = entry->index;
	command[2] = 0x00;
	command[3] = block_index & 0xFF;
	command[4] = 0x00;
	command[5] = chunks & 0xFF;
	memcpy (command + 6, CR5L_NAMESPACE, CR5L_NAMESPACE_SIZE);
	command[6 + CR5L_NAMESPACE_SIZE] = 0x00;

	dc_status_t status = crest_cr5l_send (device, command, sizeof(command));
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned char packet[CR5L_PACKET_MAX] = {0};
	unsigned int transferred = 0;
	status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred >= 1 && packet[0] == 0x39)
		return DC_STATUS_SUCCESS;

	/* Live CR5L traffic may start streaming block data immediately after the
	 * 0x39 request without a standalone 0x39 acknowledgement packet. Preserve
	 * the first chunk so the download path can consume it normally. */
	if (transferred < 2 || packet[0] != 0x00)
		return DC_STATUS_PROTOCOL;

	memcpy (device->pending_packet, packet, transferred);
	device->pending_size = transferred;
	device->pending_available = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_download_block (crest_cr5l_device_t *device, dc_buffer_t *buffer, unsigned int chunks)
{
	unsigned char packet[CR5L_PACKET_MAX] = {0};
	for (unsigned int i = 0; i < chunks; ++i) {
		unsigned int transferred = 0;
		dc_status_t status = DC_STATUS_SUCCESS;

		if (device->pending_available) {
			memcpy (packet, device->pending_packet, device->pending_size);
			transferred = device->pending_size;
			device->pending_available = 0;
			device->pending_size = 0;
		} else {
			status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
			if (status != DC_STATUS_SUCCESS)
				return status;
		}

		if (transferred < 2 || packet[0] != (i & 0xFF))
			return DC_STATUS_PROTOCOL;

		if (!dc_buffer_append (buffer, packet + 1, transferred - 1))
			return DC_STATUS_NOMEMORY;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_finish_block (crest_cr5l_device_t *device)
{
	unsigned char packet[CR5L_PACKET_MAX] = {0};
	unsigned int transferred = 0;
	dc_status_t status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
	if (status == DC_STATUS_TIMEOUT)
		return DC_STATUS_SUCCESS;
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (transferred < 1 || packet[0] != 0x39)
		return DC_STATUS_PROTOCOL;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_close_dive (crest_cr5l_device_t *device, const crest_cr5l_entry_t *entry)
{
	/* 0x32 close packets reuse the same namespace + dive-id layout as the
	 * corresponding 0x33 open request. */
	unsigned char command[2 + CR5L_NAMESPACE_SIZE + 1 + CR5L_FP_SIZE] = {0};
	unsigned int offset = 0;
	command[offset++] = 0x32;
	command[offset++] = 0x00;
	memcpy (command + offset, CR5L_NAMESPACE, CR5L_NAMESPACE_SIZE);
	offset += CR5L_NAMESPACE_SIZE;
	command[offset++] = 0x00;
	memcpy (command + offset, entry->dive_id, CR5L_FP_SIZE);
	offset += CR5L_FP_SIZE;

	dc_status_t status = crest_cr5l_send (device, command, offset);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned char packet[CR5L_PACKET_MAX] = {0};
	unsigned int transferred = 0;
	unsigned int seen = 0;

	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS)
		return status;

	while (1) {
		status = crest_cr5l_recv (device, packet, sizeof(packet), &transferred);
		if (status == DC_STATUS_TIMEOUT)
			break;
		if (status != DC_STATUS_SUCCESS)
			return status;
		if (transferred < 2 || packet[0] != 0x32)
			return DC_STATUS_PROTOCOL;

		seen++;
		if (transferred == 2 && packet[1] == 0x00)
			break;
	}

	status = dc_iostream_set_timeout (device->iostream, 5000);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return seen ? DC_STATUS_SUCCESS : DC_STATUS_PROTOCOL;
}

static dc_status_t
crest_cr5l_download_dive (crest_cr5l_device_t *device, const crest_cr5l_entry_t *entry, dc_buffer_t *buffer)
{
	dc_status_t status = crest_cr5l_open_dive (device, entry);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned int total_chunks = (entry->size + CR5L_CHUNK_DATA_SIZE - 1) / CR5L_CHUNK_DATA_SIZE;
	unsigned int remaining = total_chunks;
	unsigned int block_index = 0;

	dc_buffer_clear (buffer);
	if (!dc_buffer_reserve (buffer, entry->size))
		return DC_STATUS_NOMEMORY;

	while (remaining > 0) {
		unsigned int chunks = remaining > CR5L_BLOCK_CHUNKS ? CR5L_BLOCK_CHUNKS : remaining;
		/* In this protocol, zero encodes a full 256-chunk block instead of an
		 * empty request. */
		status = crest_cr5l_request_block (device, entry, block_index, chunks == CR5L_BLOCK_CHUNKS ? 0 : chunks);
		if (status != DC_STATUS_SUCCESS)
			return status;

		status = crest_cr5l_download_block (device, buffer, chunks);
		if (status != DC_STATUS_SUCCESS)
			return status;

		status = crest_cr5l_finish_block (device);
		if (status != DC_STATUS_SUCCESS)
			return status;

		remaining -= chunks;
		block_index++;
	}

	if (dc_buffer_get_size (buffer) < entry->size)
		return DC_STATUS_PROTOCOL;

	dc_buffer_slice (buffer, 0, entry->size);

	return crest_cr5l_close_dive (device, entry);
}

dc_status_t
crest_cr5l_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	crest_cr5l_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	device = (crest_cr5l_device_t *) dc_device_allocate (context, &crest_cr5l_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->pending_size = 0;
	device->pending_available = 0;

	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	status = dc_iostream_set_timeout (device->iostream, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_free;
	}

	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;
	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
crest_cr5l_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	crest_cr5l_device_t *device = (crest_cr5l_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	crest_cr5l_device_t *device = (crest_cr5l_device_t *) abstract;
	crest_cr5l_entry_t entries[64] = {{0}};
	unsigned int ndives = 0;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	status = crest_cr5l_emit_devinfo (device);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = crest_cr5l_send_session_init (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to initialize the CR5L transfer session.");
		return status;
	}

	/* The official app repeats the version/serial queries after session init
	 * and before dive enumeration. Mirror that ordering for compatibility. */
	status = crest_cr5l_emit_devinfo (device);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = crest_cr5l_list_dives (device, entries, sizeof(entries) / sizeof(entries[0]), &ndives);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to enumerate the dive list.");
		return status;
	}

	progress.maximum = ndives ? ndives * NSTEPS : NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	for (unsigned int i = 0; i < ndives; ++i) {
		const crest_cr5l_entry_t *entry = &entries[ndives - 1 - i];

		if (memcmp (entry->dive_id, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		progress.current = i * NSTEPS + STEP(1, 2);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		status = crest_cr5l_download_dive (device, entry, buffer);
		if (status != DC_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return status;
		}

		progress.current = (i + 1) * NSTEPS;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);
		if (callback && !callback (data, size, entry->dive_id, sizeof(entry->dive_id), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	dc_buffer_free (buffer);
	return DC_STATUS_SUCCESS;
}
