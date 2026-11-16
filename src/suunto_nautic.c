/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Jef Driesen
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

#include "suunto_nautic.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"
#include "hdlc.h"
#include "heatshrink/heatshrink_decoder.h"

#include <libdivecomputer/datetime.h>

// See suunto_nautic.h for a description of the transport and format.

#define RPC_OP_NOTIFICATION      0x01 // watch -> host: one chunk of a subscribed stream
#define RPC_OP_GET_REPLY         0x02
#define RPC_OP_RELEASE_REPLY     0x03
#define RPC_OP_DATA              0x05 // watch -> host: answers FETCH
#define RPC_OP_SUBSCRIBE_REPLY   0x08
#define RPC_OP_UNSUBSCRIBE_REPLY 0x09
#define RPC_OP_GET               0x0A // host -> watch: resolve a path to a handle
#define RPC_OP_RELEASE           0x0B // host -> watch: release a handle bound by a GET
#define RPC_OP_FETCH             0x0D
#define RPC_OP_SUBSCRIBE         0x10
#define RPC_OP_UNSUBSCRIBE       0x11
#define RPC_OP_HELLO             0x12
#define RPC_OP_HELLO_REPLY       0x13

// Packet: 0xA5 OPCODE LENGTH(2) SEQNUM(2) PAYLOAD(LENGTH) CRC32(4)
#define RPC_START    0xA5
#define RPC_OVERHEAD 10
#define MAX_PAYLOAD  512
#define MAX_PATH     240

// HTTP-like status codes carried in the replies.
#define STATUS_OK       200
#define STATUS_CONTINUE 100 // more pages follow

// A GET of a per-id path (/Logbook/byId/<id>/...) binds the id to one of the
// resource's handle slots, [00|10] 24 <resource>, until a RELEASE frees it. A
// second GET while the 00 slot is still bound, e.g. by another app on the
// link, gets the 10 slot; the handle the GET returns is always the one used.
// Static paths answer with an F0 handle and an unknown path with FF FF FF;
// neither is released. The slot is the first byte on the wire.
#define HANDLE_SLOT(handle) ((handle) & 0xFF)
#define SLOT_STATIC  0xF0
#define SLOT_UNKNOWN 0xFF

// Request parameters are COUNT(1), then TYPE(2) VALUE for each parameter. The
// /Summary page offset is an int32, the /Logbook/Entries StartAfterId (the id
// of the last entry received) a uint32.
#define PARAM_INT32  0x06
#define PARAM_UINT32 0x07

// Reply payloads start with HANDLE(3) CLIENT(3) STATUS(2), where CLIENT is
// the 01 80 00 of the request; a DATA reply then carries the content.
#define REPLY_STATUS  6
#define REPLY_CONTENT 8

// Stream notification payload:
// HANDLE(3) CLIENT(3) UNKNOWN(4) OFFSET(4) LENGTH(2) UNKNOWN(6) DATA(LENGTH) 00
// OFFSET is the position of the chunk in the stream. The last chunk has a
// zero length, at an offset equal to the total size, and is sent twice.
#define CHUNK_OFFSET 10
#define CHUNK_LENGTH 14
#define CHUNK_DATA   22

// A /Summary page is HEADER(11) DATA, with the data length at offset 3 of
// the header. The paging offset counts data bytes only.
#define PAGE_HEADER 11
#define PAGE_LENGTH 3

// A /Logbook/Entries page is HEADER(18) RECORD(24) x COUNT, with the number
// of records in the page as a uint16 at offset 6 of the header. A record is
// ID(4) MODIFIED(4) UNKNOWN(8) SIZE(8): the id is the dive's start time, and
// the size is the compressed /Data plus the /Summary data bytes. An empty
// page has a 14-byte header, without the 4 padding bytes before the records.
#define ENTRIES_HEADER 18
#define ENTRIES_EMPTY  14
#define ENTRIES_COUNT  6
#define ENTRY_SIZE     24
#define ENTRY_START    0
#define ENTRY_LENGTH   16

// How many times to issue a dive's stream when it fails with a protocol error
// (e.g. status 423 (Locked), or a lost chunk) or stops before its end marker,
// and to download a dive whose size doesn't match the listing.
#define STREAM_ATTEMPTS   3
#define DOWNLOAD_ATTEMPTS 2

#define TIMEOUT_DEFAULT 5000
#define TIMEOUT_STREAM  4000 // the gap between two chunks reaches about 2s

// Heatshrink (LZSS) parameters used by the Nautic/Ocean's MDS stream.
#define HEATSHRINK_WINDOW_SZ2    7
#define HEATSHRINK_LOOKAHEAD_SZ2 5
#define HEATSHRINK_INPUT_BUFFER_SIZE 256

static const unsigned char SBEM_MAGIC[8] = {'S','B','E','M','0','1','0','3'};

typedef struct suunto_nautic_device_t {
	dc_device_t base;
	dc_iostream_t *iostream; // HDLC-framed
	unsigned int seqnum;
	// The dive id (a UNIX timestamp) of the most recently downloaded dive,
	// little-endian. All-zero means no fingerprint.
	unsigned char fingerprint[4];
} suunto_nautic_device_t;

static dc_status_t suunto_nautic_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t suunto_nautic_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_nautic_device_close (dc_device_t *abstract);

static const dc_device_vtable_t suunto_nautic_device_vtable = {
	sizeof(suunto_nautic_device_t),
	DC_FAMILY_SUUNTO_NAUTIC,
	suunto_nautic_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	suunto_nautic_device_foreach, /* foreach */
	NULL, /* timesync */
	suunto_nautic_device_close, /* close */
};

// The Hello payload: the protocol versions (9, 9), the host identity (the one
// the Suunto app sends, a packed 12-byte serial), the Whiteboard version
// (4.1.2) and capability flags. The watch answers with the same fields for
// itself.
static const unsigned char suunto_nautic_hello[] = {
	0x09, 0x09, 0x20, 0x16, 0x45, 0x56, 0x41, 0x10,
	0x04, 0x41, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x04,
	0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char suunto_nautic_noparams[] = {0x00};

static dc_status_t
suunto_nautic_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static unsigned int
suunto_nautic_seqnum (suunto_nautic_device_t *device)
{
	// Zero is the sequence number of the stream notifications.
	device->seqnum = (device->seqnum + 1) & 0xFFFF;
	if (device->seqnum == 0)
		device->seqnum = 1;

	return device->seqnum;
}

static dc_status_t
suunto_nautic_send (suunto_nautic_device_t *device, unsigned char opcode, unsigned int seqnum, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char packet[RPC_OVERHEAD + MAX_PAYLOAD] = {0};

	if (size > MAX_PAYLOAD)
		return DC_STATUS_INVALIDARGS;

	packet[0] = RPC_START;
	packet[1] = opcode;
	array_uint16_le_set (packet + 2, size);
	array_uint16_le_set (packet + 4, seqnum);
	if (size)
		memcpy (packet + 6, data, size);

	unsigned int crc = checksum_crc32r (packet, 6 + size);
	array_uint32_le_set (packet + 6 + size, crc);

	status = dc_iostream_write (device->iostream, packet, RPC_OVERHEAD + size, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the packet.");
		return status;
	}

	return status;
}

// Receive the packet with the given opcode and sequence number. The watch
// serves every app connected to it over the same link, so the replies to
// another app's requests can arrive in between. Those are recognized by their
// sequence number and skipped, and so are the notifications of a stream that
// has ended, such as its repeated end marker.
static dc_status_t
suunto_nautic_recv (suunto_nautic_device_t *device, unsigned char opcode, unsigned int seqnum, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char packet[RPC_OVERHEAD + MAX_PAYLOAD] = {0};
	size_t length = 0;

	if (size > MAX_PAYLOAD)
		return DC_STATUS_INVALIDARGS;

	while (1) {
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), &length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (device->base.context, "Failed to receive the packet.");
			return status;
		}

		if (length == 0) {
			// Two flags in a row, e.g. after the tail of a packet that was
			// cut off when an earlier link dropped.
			continue;
		}

		if (length < RPC_OVERHEAD) {
			ERROR (device->base.context, "Unexpected packet size (" DC_PRINTF_SIZE ").", length);
			return DC_STATUS_PROTOCOL;
		}

		unsigned int crc = array_uint32_le (packet + length - 4);
		unsigned int ccrc = checksum_crc32r (packet, length - 4);
		if (crc != ccrc) {
			ERROR (device->base.context, "Unexpected packet checksum (%08x %08x).", crc, ccrc);
			return DC_STATUS_PROTOCOL;
		}

		if (packet[0] != RPC_START) {
			ERROR (device->base.context, "Unexpected packet start byte (%02x).", packet[0]);
			return DC_STATUS_PROTOCOL;
		}

		unsigned int rsp_length = array_uint16_le (packet + 2);
		if (rsp_length != length - RPC_OVERHEAD) {
			ERROR (device->base.context, "Unexpected packet payload length (%u " DC_PRINTF_SIZE ").", rsp_length, length - RPC_OVERHEAD);
			return DC_STATUS_PROTOCOL;
		}

		unsigned int rsp_opcode = packet[1];
		unsigned int rsp_seqnum = array_uint16_le (packet + 4);
		if (rsp_opcode != opcode || rsp_seqnum != seqnum) {
			WARNING (device->base.context, "Skipping an unexpected packet (%02x %u).", rsp_opcode, rsp_seqnum);
			continue;
		}

		if (rsp_length > size) {
			ERROR (device->base.context, "Unexpected packet payload length (%u " DC_PRINTF_SIZE ").", rsp_length, size);
			return DC_STATUS_PROTOCOL;
		}

		memcpy (data, packet + 6, rsp_length);
		if (actual)
			*actual = rsp_length;

		return status;
	}
}

// Send a request on a handle and receive its reply: the payload is the handle
// and the 01 80 00 client id, followed by the parameters, if any.
static dc_status_t
suunto_nautic_transfer (suunto_nautic_device_t *device, unsigned char opcode, unsigned char rsp_opcode,
	unsigned int handle, const unsigned char params[], size_t nparams, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char request[6 + 7] = {0};

	if (nparams > sizeof(request) - 6)
		return DC_STATUS_INVALIDARGS;

	array_uint24_le_set (request, handle);
	request[3] = 0x01;
	request[4] = 0x80;
	request[5] = 0x00;
	if (nparams)
		memcpy (request + 6, params, nparams);

	unsigned int seqnum = suunto_nautic_seqnum (device);
	status = suunto_nautic_send (device, opcode, seqnum, request, 6 + nparams);
	if (status != DC_STATUS_SUCCESS)
		return status;

	size_t length = 0;
	status = suunto_nautic_recv (device, rsp_opcode, seqnum, data, size, &length);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (length < REPLY_CONTENT) {
		ERROR (device->base.context, "Unexpected reply size (" DC_PRINTF_SIZE ").", length);
		return DC_STATUS_PROTOCOL;
	}

	if (actual)
		*actual = length;

	return status;
}

dc_status_t
suunto_nautic_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	device = (suunto_nautic_device_t *) dc_device_allocate (context, &suunto_nautic_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Start at a different sequence number on every connection, so our
	// requests are unlikely to share a number with those of another app on
	// the same link, whose replies we also receive.
	device->seqnum = ((unsigned int) dc_datetime_now () * 2654435761u) & 0xFFFF;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	status = dc_hdlc_open (&device->iostream, context, iostream, 244, 244);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the HDLC layer.");
		goto error_free;
	}

	status = dc_iostream_set_timeout (device->iostream, TIMEOUT_DEFAULT);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	status = suunto_nautic_send (device, RPC_OP_HELLO, 0, suunto_nautic_hello, sizeof (suunto_nautic_hello));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the hello.");
		goto error_close;
	}

	unsigned char hello[sizeof (suunto_nautic_hello)] = {0};
	size_t hello_len = 0;
	status = suunto_nautic_recv (device, RPC_OP_HELLO_REPLY, 0, hello, sizeof (hello), &hello_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to receive the hello.");
		goto error_close;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_iostream_close (device->iostream);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
suunto_nautic_device_close (dc_device_t *abstract)
{
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	return dc_iostream_close (device->iostream);
}

// Release a handle bound by a GET, so the next GET of the same resource binds
// its own id to the slot.
static dc_status_t
suunto_nautic_release (suunto_nautic_device_t *device, unsigned int handle)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (HANDLE_SLOT (handle) == SLOT_STATIC || HANDLE_SLOT (handle) == SLOT_UNKNOWN)
		return status;

	unsigned char reply[REPLY_CONTENT] = {0};
	status = suunto_nautic_transfer (device, RPC_OP_RELEASE, RPC_OP_RELEASE_REPLY,
		handle, NULL, 0, reply, sizeof (reply), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to release handle %06x.", handle);
		return status;
	}

	unsigned int errcode = array_uint16_le (reply + REPLY_STATUS);
	if (errcode != STATUS_OK) {
		ERROR (device->base.context, "Failed to release handle %06x (status %u).", handle, errcode);
		return DC_STATUS_PROTOCOL;
	}

	return status;
}

// Resolve a path to a handle. The GET reply is HANDLE(3) FLAGS(3) STATUS(2).
static dc_status_t
suunto_nautic_get_handle (suunto_nautic_device_t *device, const char *path, unsigned int *handle)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (path == NULL)
		return DC_STATUS_INVALIDARGS;

	size_t pathlen = strlen (path);
	if (pathlen == 0 || pathlen > MAX_PATH)
		return DC_STATUS_INVALIDARGS;

	unsigned char request[4 + MAX_PATH] = {0};
	request[0] = 0x01;
	request[1] = 0x80;
	request[2] = 0x00;
	request[3] = pathlen;
	memcpy (request + 4, path, pathlen);

	unsigned int seqnum = suunto_nautic_seqnum (device);
	status = suunto_nautic_send (device, RPC_OP_GET, seqnum, request, 4 + pathlen);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the request for %s.", path);
		return status;
	}

	unsigned char reply[REPLY_CONTENT] = {0};
	size_t reply_len = 0;
	status = suunto_nautic_recv (device, RPC_OP_GET_REPLY, seqnum, reply, sizeof (reply), &reply_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to receive the reply for %s.", path);
		return status;
	}

	if (reply_len != sizeof (reply)) {
		ERROR (device->base.context, "Unexpected reply size (" DC_PRINTF_SIZE ").", reply_len);
		return DC_STATUS_PROTOCOL;
	}

	unsigned int value = array_uint24_le (reply);
	unsigned int errcode = array_uint16_le (reply + REPLY_STATUS);
	if (errcode != STATUS_OK) {
		ERROR (device->base.context, "Failed to resolve %s (status %u).", path, errcode);
		return DC_STATUS_PROTOCOL;
	}

	if (handle)
		*handle = value;

	return status;
}

// Decompress a Heatshrink (LZSS) stream, with the parameters above.
static dc_status_t
suunto_nautic_heatshrink_decompress (dc_context_t *context, const unsigned char *input, size_t input_size, dc_buffer_t *output)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char outbuf[HEATSHRINK_INPUT_BUFFER_SIZE];

	heatshrink_decoder *hsd = heatshrink_decoder_alloc (HEATSHRINK_INPUT_BUFFER_SIZE, HEATSHRINK_WINDOW_SZ2, HEATSHRINK_LOOKAHEAD_SZ2);
	if (hsd == NULL) {
		ERROR (context, "Failed to allocate the heatshrink decoder.");
		return DC_STATUS_NOMEMORY;
	}

	dc_buffer_clear (output);

	size_t sunk_total = 0;
	while (sunk_total < input_size) {
		size_t sunk = 0;
		HSD_sink_res sres = heatshrink_decoder_sink (hsd, (uint8_t *) input + sunk_total, input_size - sunk_total, &sunk);
		if (sres < 0) {
			ERROR (context, "Heatshrink sink error (%d).", sres);
			status = DC_STATUS_DATAFORMAT;
			goto done;
		}
		sunk_total += sunk;

		HSD_poll_res pres;
		do {
			size_t polled = 0;
			pres = heatshrink_decoder_poll (hsd, outbuf, sizeof (outbuf), &polled);
			if (pres < 0) {
				ERROR (context, "Heatshrink poll error (%d).", pres);
				status = DC_STATUS_DATAFORMAT;
				goto done;
			}
			if (polled && !dc_buffer_append (output, outbuf, polled)) {
				ERROR (context, "Failed to allocate memory.");
				status = DC_STATUS_NOMEMORY;
				goto done;
			}
		} while (pres == HSDR_POLL_MORE);
	}

	HSD_finish_res fres = heatshrink_decoder_finish (hsd);
	while (fres == HSDR_FINISH_MORE) {
		HSD_poll_res pres;
		do {
			size_t polled = 0;
			pres = heatshrink_decoder_poll (hsd, outbuf, sizeof (outbuf), &polled);
			if (pres < 0) {
				ERROR (context, "Heatshrink poll error (%d).", pres);
				status = DC_STATUS_DATAFORMAT;
				goto done;
			}
			if (polled && !dc_buffer_append (output, outbuf, polled)) {
				ERROR (context, "Failed to allocate memory.");
				status = DC_STATUS_NOMEMORY;
				goto done;
			}
		} while (pres == HSDR_POLL_MORE);
		fres = heatshrink_decoder_finish (hsd);
	}

done:
	heatshrink_decoder_free (hsd);
	return status;
}

// Download a streamed resource (the dive profile): GET, SUBSCRIBE, the
// notifications up to the end marker, UNSUBSCRIBE and RELEASE. Returns the
// still compressed stream. Returns DC_STATUS_PROTOCOL on a protocol error,
// e.g. when the watch refuses the stream (status 423 while the previous
// stream's handle is still bound) or a chunk is missing, and
// DC_STATUS_TIMEOUT when the stream stops before its end marker.
static dc_status_t
suunto_nautic_stream (suunto_nautic_device_t *device, const char *path, dc_buffer_t *buffer)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned char reply[REPLY_CONTENT + 2] = {0};
	size_t reply_len = 0;
	status = suunto_nautic_transfer (device, RPC_OP_SUBSCRIBE, RPC_OP_SUBSCRIBE_REPLY,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), &reply_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to subscribe to %s.", path);
		goto error_release;
	}

	unsigned int result = array_uint16_le (reply + REPLY_STATUS);
	if (result != STATUS_OK) {
		ERROR (abstract->context, "The watch refused the stream for %s (status %u).", path, result);
		status = DC_STATUS_PROTOCOL;
		goto error_release;
	}

	status = dc_iostream_set_timeout (device->iostream, TIMEOUT_STREAM);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the timeout.");
		goto error_unsubscribe;
	}

	// The watch streams the whole resource without acknowledgements, up to
	// the end marker.
	dc_buffer_clear (buffer);
	while (1) {
		if (device_is_cancelled (abstract)) {
			status = DC_STATUS_CANCELLED;
			break;
		}

		unsigned char chunk[MAX_PAYLOAD] = {0};
		size_t chunk_len = 0;
		status = suunto_nautic_recv (device, RPC_OP_NOTIFICATION, 0, chunk, sizeof (chunk), &chunk_len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the stream for %s.", path);
			break;
		}

		if (chunk_len < CHUNK_DATA) {
			ERROR (abstract->context, "Unexpected chunk size (" DC_PRINTF_SIZE ").", chunk_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		unsigned int offset = array_uint32_le (chunk + CHUNK_OFFSET);
		unsigned int length = array_uint16_le (chunk + CHUNK_LENGTH);
		if (CHUNK_DATA + length > chunk_len) {
			ERROR (abstract->context, "Unexpected chunk length (%u " DC_PRINTF_SIZE ").", length, chunk_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		size_t received = dc_buffer_get_size (buffer);
		if (offset != received) {
			ERROR (abstract->context, "Unexpected chunk offset (%u " DC_PRINTF_SIZE ").", offset, received);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (length == 0)
			break;

		if (!dc_buffer_append (buffer, chunk + CHUNK_DATA, length)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}
	}

error_unsubscribe:
	// Without the UNSUBSCRIBE the watch keeps streaming, and the next GET of a
	// stream is refused with status 423.
	rc = suunto_nautic_transfer (device, RPC_OP_UNSUBSCRIBE, RPC_OP_UNSUBSCRIBE_REPLY,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), &reply_len);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to unsubscribe from %s.", path);
		dc_status_set_error (&status, rc);
	} else {
		unsigned int errcode = array_uint16_le (reply + REPLY_STATUS);
		if (errcode != STATUS_OK) {
			ERROR (abstract->context, "Failed to unsubscribe from %s (status %u).", path, errcode);
			dc_status_set_error (&status, DC_STATUS_PROTOCOL);
		}
	}

	rc = dc_iostream_set_timeout (device->iostream, TIMEOUT_DEFAULT);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to restore the timeout.");
		dc_status_set_error (&status, rc);
	}

error_release:
	rc = suunto_nautic_release (device, handle);
	if (rc != DC_STATUS_SUCCESS)
		dc_status_set_error (&status, rc);
	return status;
}

// Read a paged resource (/Summary). Each page is requested by the offset of
// its data, and the last page has status 200.
static dc_status_t
suunto_nautic_fetch_pages (suunto_nautic_device_t *device, const char *path, dc_buffer_t *buffer)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	dc_buffer_clear (buffer);
	unsigned int offset = 0;
	while (1) {
		unsigned char params[7] = {0x01, PARAM_INT32, 0x00};
		array_uint32_le_set (params + 3, offset);

		unsigned char reply[MAX_PAYLOAD] = {0};
		size_t reply_len = 0;
		status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
			handle, params, sizeof (params), reply, sizeof (reply), &reply_len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to fetch %s (offset %u).", path, offset);
			break;
		}

		unsigned int result = array_uint16_le (reply + REPLY_STATUS);
		if (result != STATUS_OK && result != STATUS_CONTINUE) {
			ERROR (abstract->context, "Failed to fetch %s (status %u).", path, result);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (reply_len < REPLY_CONTENT + PAGE_HEADER) {
			ERROR (abstract->context, "Unexpected page size (" DC_PRINTF_SIZE ").", reply_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		unsigned int length = array_uint16_le (reply + REPLY_CONTENT + PAGE_LENGTH);
		if (REPLY_CONTENT + PAGE_HEADER + length > reply_len) {
			ERROR (abstract->context, "Unexpected page length (%u " DC_PRINTF_SIZE ").", length, reply_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (!dc_buffer_append (buffer, reply + REPLY_CONTENT + PAGE_HEADER, length)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}
		offset += length;

		if (result == STATUS_OK)
			break;

		// The offset didn't advance, so the next request would be the same
		// one again.
		if (length == 0) {
			ERROR (abstract->context, "Empty page with more pages pending.");
			status = DC_STATUS_PROTOCOL;
			break;
		}
	}

	dc_status_t rc = suunto_nautic_release (device, handle);
	if (rc != DC_STATUS_SUCCESS)
		dc_status_set_error (&status, rc);
	return status;
}

// Read a small resource that fits in a single reply (/Info).
static dc_status_t
suunto_nautic_fetch (suunto_nautic_device_t *device, const char *path, dc_buffer_t *buffer)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char reply[MAX_PAYLOAD] = {0};
	size_t reply_len = 0;
	status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), &reply_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to fetch %s.", path);
		goto error_release;
	}

	unsigned int result = array_uint16_le (reply + REPLY_STATUS);
	if (result != STATUS_OK) {
		ERROR (abstract->context, "Failed to fetch %s (status %u).", path, result);
		status = DC_STATUS_PROTOCOL;
		goto error_release;
	}

	dc_buffer_clear (buffer);
	if (!dc_buffer_append (buffer, reply + REPLY_CONTENT, reply_len - REPLY_CONTENT)) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
	}

error_release:
	rc = suunto_nautic_release (device, handle);
	if (rc != DC_STATUS_SUCCESS)
		dc_status_set_error (&status, rc);
	return status;
}

// List the dives: the records of all pages, oldest first. The list is served
// in pages: a page with status 100 is followed by a fetch for the dives after
// the last id received, until a page with status 200.
static dc_status_t
suunto_nautic_list (suunto_nautic_device_t *device, dc_buffer_t *entries)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;
	const char *path = "/Logbook/Entries";

	dc_buffer_clear (entries);

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// The first page is requested without the StartAfterId, which is
	// optional, as the Suunto app does.
	unsigned int last = 0;
	for (unsigned int page = 0; ; page++) {
		unsigned char params[7] = {0x00, PARAM_UINT32, 0x00};
		size_t nparams = 1;
		if (page > 0) {
			params[0] = 0x01;
			array_uint32_le_set (params + 3, last);
			nparams = sizeof (params);
		}

		unsigned char reply[MAX_PAYLOAD] = {0};
		size_t reply_len = 0;
		status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
			handle, params, nparams, reply, sizeof (reply), &reply_len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to fetch %s.", path);
			break;
		}

		unsigned int result = array_uint16_le (reply + REPLY_STATUS);
		if (result != STATUS_OK && result != STATUS_CONTINUE) {
			ERROR (abstract->context, "Failed to fetch %s (status %u).", path, result);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		const unsigned char *data = reply + REPLY_CONTENT;
		size_t size = reply_len - REPLY_CONTENT;
		if (size < ENTRIES_EMPTY) {
			ERROR (abstract->context, "Unexpected entries size (" DC_PRINTF_SIZE ").", size);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		unsigned int count = array_uint16_le (data + ENTRIES_COUNT);
		if (count && ENTRIES_HEADER + count * ENTRY_SIZE > size) {
			ERROR (abstract->context, "Unexpected number of entries (%u " DC_PRINTF_SIZE ").", count, size);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (!dc_buffer_append (entries, data + ENTRIES_HEADER, count * ENTRY_SIZE)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}

		unsigned int previous = last;
		if (count)
			last = array_uint32_le (data + ENTRIES_HEADER + (count - 1) * ENTRY_SIZE + ENTRY_START);

		if (result == STATUS_OK)
			break;

		// The id didn't advance, so the next request would be the same one
		// again.
		if (last == previous) {
			ERROR (abstract->context, "No new entries with more pages pending.");
			status = DC_STATUS_PROTOCOL;
			break;
		}
	}

	dc_status_t rc = suunto_nautic_release (device, handle);
	if (rc != DC_STATUS_SUCCESS)
		dc_status_set_error (&status, rc);

	if (status != DC_STATUS_SUCCESS)
		dc_buffer_clear (entries);

	return status;
}

// Download one dive: the decompressed /Data stream with the /Summary
// appended. The listed size is the compressed /Data plus the /Summary data
// bytes, exactly. A mismatch is retried once and then fails the download.
// `unreadable` is set when the transfer matched the listing but holds no dive
// (an empty or aborted logbook entry), so downloading it again cannot help.
static dc_status_t
suunto_nautic_download (suunto_nautic_device_t *device, unsigned int id, unsigned int listed,
	dc_buffer_t *buffer, int *unreadable)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;
	int incomplete = 0;

	*unreadable = 0;

	char data_path[64], summary_path[64];
	snprintf (data_path, sizeof (data_path), "/Logbook/byId/%u/Data", id);
	snprintf (summary_path, sizeof (summary_path), "/Logbook/byId/%u/Summary", id);

	dc_buffer_t *compressed = dc_buffer_new (listed);
	dc_buffer_t *summary = dc_buffer_new (0);
	if (compressed == NULL || summary == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free;
	}

	for (unsigned int attempt = 0; attempt < DOWNLOAD_ATTEMPTS; attempt++) {
		for (unsigned int i = 0; i < STREAM_ATTEMPTS; i++) {
			if (i > 0) {
				WARNING (abstract->context, "Retrying the stream of dive %u.", id);
				dc_iostream_sleep (device->iostream, 1500 * i);
			}
			status = suunto_nautic_stream (device, data_path, compressed);
			if (status != DC_STATUS_PROTOCOL && status != DC_STATUS_TIMEOUT)
				break;
		}
		if (status != DC_STATUS_SUCCESS)
			goto error_free;

		status = suunto_nautic_fetch_pages (device, summary_path, summary);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to fetch the summary of dive %u.", id);
			goto error_free;
		}

		if (listed == 0)
			break;

		size_t data_size = dc_buffer_get_size (compressed);
		size_t summary_size = dc_buffer_get_size (summary);
		incomplete = data_size + summary_size != listed;
		if (!incomplete)
			break;

		WARNING (abstract->context, "Incomplete download of dive %u (" DC_PRINTF_SIZE " + " DC_PRINTF_SIZE " bytes, listed %u).",
			id, data_size, summary_size, listed);
	}

	if (incomplete) {
		ERROR (abstract->context, "Failed to download dive %u completely.", id);
		status = DC_STATUS_PROTOCOL;
		goto error_free;
	}

	status = suunto_nautic_heatshrink_decompress (abstract->context,
		dc_buffer_get_data (compressed), dc_buffer_get_size (compressed), buffer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to decompress dive %u.", id);
		*unreadable = status == DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	if (dc_buffer_get_size (buffer) < sizeof (SBEM_MAGIC) ||
		memcmp (dc_buffer_get_data (buffer), SBEM_MAGIC, sizeof (SBEM_MAGIC)) != 0) {
		ERROR (abstract->context, "Unexpected magic in dive %u.", id);
		status = DC_STATUS_DATAFORMAT;
		*unreadable = 1;
		goto error_free;
	}

	// The /Summary goes after the profile, so the parser can read the fields
	// that are not in the profile (gradient factors, gas mixes).
	if (!dc_buffer_append (buffer, dc_buffer_get_data (summary), dc_buffer_get_size (summary))) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
	}

error_free:
	if (status != DC_STATUS_SUCCESS)
		dc_buffer_clear (buffer);
	dc_buffer_free (summary);
	dc_buffer_free (compressed);
	return status;
}

// True for a token that is exactly "<digits>.<digits>.<digits>", each part
// < 256; on success fills a/b/c.
static int
suunto_nautic_parse_version (const char *tok, size_t len, unsigned int *a, unsigned int *b, unsigned int *c)
{
	unsigned int part[3] = {0}, idx = 0, digits = 0;
	for (size_t i = 0; i < len; i++) {
		char ch = tok[i];
		if (ch >= '0' && ch <= '9') {
			part[idx] = part[idx] * 10 + (unsigned int) (ch - '0');
			if (part[idx] > 255 || ++digits > 3)
				return 0;
		} else if (ch == '.') {
			if (digits == 0 || ++idx > 2)
				return 0;
			digits = 0;
		} else {
			return 0;
		}
	}
	if (idx != 2 || digits == 0)
		return 0;
	*a = part[0]; *b = part[1]; *c = part[2];
	return 1;
}

// True for a token that is exactly 12 chars, all [0-9A-F] -- the watch serial
// form, e.g. "2604C3003306".
static int
suunto_nautic_is_serial (const char *tok, size_t len)
{
	if (len != 12)
		return 0;
	for (size_t i = 0; i < len; i++) {
		char ch = tok[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')))
			return 0;
	}
	return 1;
}

// /Info is a run of NUL-terminated strings mixed with binary fields:
//
//   Suunto\0 Nautic\0 Vaasa\0 T\0 2604C3003306\0 ... 2.55.46\0 ...
//     BLE Mac Address\0 <mac>\0 WiFi Mac Address\0 <mac>\0
//
// The serial is the first 12-character uppercase hex string, and the firmware
// the first "N.N.N" string, which follows the serial. The MAC addresses after
// it are also 12 hex characters. The serial is reported as its low 32 bits.
static dc_status_t
suunto_nautic_devinfo (suunto_nautic_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;

	dc_buffer_t *info = dc_buffer_new (0);
	if (info == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	status = suunto_nautic_fetch (device, "/Info", info);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the device info.");
		dc_buffer_free (info);
		return status;
	}

	const unsigned char *data = dc_buffer_get_data (info);
	size_t size = dc_buffer_get_size (info);

	dc_event_devinfo_t devinfo;
	memset (&devinfo, 0, sizeof (devinfo));

	size_t start = 0;
	for (size_t i = 0; i < size; i++) {
		if (data[i] != 0)
			continue;
		const char *token = (const char *) (data + start);
		size_t length = i - start;
		unsigned int a, b, c;
		if (!devinfo.firmware && suunto_nautic_parse_version (token, length, &a, &b, &c)) {
			devinfo.firmware = (a << 16) | (b << 8) | c;
		} else if (!devinfo.firmware && !devinfo.serial && suunto_nautic_is_serial (token, length)) {
			char serial[13];
			memcpy (serial, token, 12);
			serial[12] = 0;
			devinfo.serial = (unsigned int) strtoul (serial, NULL, 16);
		}
		start = i + 1;
	}

	dc_buffer_free (info);

	if (devinfo.firmware == 0 || devinfo.serial == 0) {
		ERROR (abstract->context, "Failed to find the firmware and serial in the device info.");
		return DC_STATUS_DATAFORMAT;
	}

	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return status;
}

static dc_status_t
suunto_nautic_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	status = suunto_nautic_devinfo (device);
	if (status != DC_STATUS_SUCCESS)
		return status;

	progress.current = 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *entries = dc_buffer_new (0);
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (entries == NULL || buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free;
	}

	status = suunto_nautic_list (device, entries);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to list the dives.");
		goto error_free;
	}

	const unsigned char *records = dc_buffer_get_data (entries);
	unsigned int count = dc_buffer_get_size (entries) / ENTRY_SIZE;

	progress.maximum = (count + 1) * 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// The entries are oldest first, so process them in reverse order.
	for (unsigned int i = count; i > 0; i--) {
		const unsigned char *record = records + (i - 1) * ENTRY_SIZE;
		unsigned int id = array_uint32_le (record + ENTRY_START);
		unsigned int listed = array_uint32_le (record + ENTRY_LENGTH);

		unsigned char fingerprint[4];
		array_uint32_le_set (fingerprint, id);

		// The dives are newest first, so everything from here on was
		// downloaded before.
		if (memcmp (fingerprint, device->fingerprint, sizeof (fingerprint)) == 0)
			break;

		if (device_is_cancelled (abstract)) {
			status = DC_STATUS_CANCELLED;
			break;
		}

		int unreadable = 0;
		status = suunto_nautic_download (device, id, listed, buffer, &unreadable);
		if (status != DC_STATUS_SUCCESS && unreadable) {
			// An empty or aborted entry: listed, but without a dive.
			WARNING (abstract->context, "Skipping dive %u without any data.", id);
			status = DC_STATUS_SUCCESS;
			continue;
		}
		if (status != DC_STATUS_SUCCESS) {
			// Stop here: the fingerprint of a newer dive would mark this one
			// as downloaded.
			ERROR (abstract->context, "Failed to download dive %u.", id);
			break;
		}

		progress.current += 2;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		if (callback && !callback (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), fingerprint, sizeof (fingerprint), userdata))
			break;
	}

error_free:
	dc_buffer_free (buffer);
	dc_buffer_free (entries);
	return status;
}
