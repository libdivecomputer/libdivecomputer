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
#define MAX_PAYLOAD  512
#define MAX_PATH     240
#define MAX_PARAMS   7

// HTTP-like status codes carried in the replies.
#define ERR_OK       200
#define ERR_CONTINUE 100 // more pages follow

// A GET of a per-id path (/Logbook/byId/<id>/...) binds the id to one of the
// resource's handle slots, [00|10] 24 <resource>, until a RELEASE frees it. A
// second GET while the 00 slot is still bound, e.g. by another app on the
// link, gets the 10 slot; the handle the GET returns is always the one used.
// Static paths answer with an F0 handle, e.g. F0 24 00 for /Logbook/Entries,
// which isn't released, and an unknown path with FF FF FF. A few static
// paths, /Info among them, answer with F1 instead; those are released like a
// slot, and the watch answers that with 200. The slot is the first byte on
// the wire.
#define HANDLE_ISSTATIC(handle)  (((handle) & 0xFF) == 0xF0)
#define HANDLE_ISINVALID(handle) ((handle) == 0xFFFFFF)

// Request parameters are COUNT(1), then TYPE(2) VALUE for each parameter. The
// /Summary page offset is an int32, the /Logbook/Entries StartAfterId (the id
// of the last entry received) a uint32.
#define PARAM_INT32  0x06
#define PARAM_UINT32 0x07

// Stream notification payload:
// HANDLE(3) CLIENT(3) UNKNOWN(4) OFFSET(4) LENGTH(2) UNKNOWN(6) DATA(LENGTH) 00
// OFFSET is the position of the chunk in the stream. The last chunk has a
// zero length, at an offset equal to the total size, and is sent twice. The
// 00 after the data ends every notification the watch sends, not only those
// of the stream.
#define CHUNK_OFFSET 10
#define CHUNK_LENGTH 14
#define CHUNK_DATA   22

// A /Summary page is HEADER(11) DATA, with the data length at offset 3 of
// the header. The paging offset counts data bytes only.
#define PAGE_HEADER 11
#define PAGE_LENGTH 3

// A /Logbook/Entries page is a Whiteboard struct, TYPE(2) SIZE(2) PADDING(2)
// STRUCT, and the struct is an array, COUNT(2) PADDING(2) OFFSET(4), with the
// records at OFFSET from the start of the struct: 12 on a page with records,
// 8 on a page without. A record is ID(4) MODIFIED(4) 01 PADDING(7) SIZE(8),
// the LogEntry of the Movesense logbook API, in which the size is optional
// and the 01 marks it as present. The id is the dive's start time, and the
// size is the compressed /Data plus the /Summary data bytes, which fits in
// its low 4 bytes.
#define ENTRIES_STRUCT  6
#define ENTRIES_COUNT   0
#define ENTRIES_RECORDS 4
#define ENTRY_SIZE     24
#define ENTRY_START    0
#define ENTRY_HASSIZE  8
#define ENTRY_LENGTH   16

// /Info is a Whiteboard struct, TYPE(2) SIZE(2) PADDING(2) STRUCT, and the
// struct starts with the offsets of eight NUL-terminated strings, as uint32
// counted from the start of the struct: the manufacturer, the product, the
// codename, a hardware id, the serial, a PCBA id, the firmware and the
// hardware version, e.g. Suunto, Nautic, Vaasa, T, 2604C3003306, ..., 2.55.46.
// The type is 02 2F on the Nautic, the Nautic S and the Ocean. The serial is
// 12 hex digits, and is reported as the value of its last 8 (0xC3003306 for
// 2604C3003306).
#define INFO_TYPE     0x2F02
#define INFO_STRUCT   6
#define INFO_SERIAL   4
#define INFO_FIRMWARE 6

#define NSTEPS 1000

#define TIMEOUT_DEFAULT 5000
#define TIMEOUT_STREAM  4000 // the gap between two chunks reaches about 2s

// Heatshrink (LZSS) parameters used by the Nautic/Ocean's MDS stream.
#define HEATSHRINK_WINDOW_SZ2    7
#define HEATSHRINK_LOOKAHEAD_SZ2 5
#define HEATSHRINK_INPUT_BUFFER_SIZE 256

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

static unsigned int
suunto_nautic_seqnum (suunto_nautic_device_t *device)
{
	// Zero is Whiteboard's invalid request id (ID_INVALID_REQUEST). The watch
	// uses it for the packets that don't answer a request: the hello, its
	// reply and the stream notifications.
	device->seqnum = (device->seqnum + 1) & 0xFFFF;
	if (device->seqnum == 0)
		device->seqnum = 1;

	return device->seqnum;
}

static dc_status_t
suunto_nautic_send (suunto_nautic_device_t *device, unsigned char opcode, unsigned int seqnum, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[6 + MAX_PAYLOAD + 4] = {0};

	// The UNSUBSCRIBE and RELEASE still go out after a cancel, so the watch
	// doesn't keep streaming or keep the handle bound.
	if (opcode != RPC_OP_UNSUBSCRIBE && opcode != RPC_OP_RELEASE &&
		device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

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

	status = dc_iostream_write (device->iostream, packet, 6 + size + 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the packet.");
		return status;
	}

	return status;
}

// Receive the packet with the given opcode and sequence number. The watch
// serves every app connected to it over the same link, so the replies to
// another app's requests can arrive in between. Those are recognized by their
// sequence number and skipped, and so are the notifications that are still
// on their way after a stream was stopped before its end.
static dc_status_t
suunto_nautic_recv (suunto_nautic_device_t *device, unsigned char opcode, unsigned int seqnum, unsigned char data[], size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char packet[6 + MAX_PAYLOAD + 4] = {0};
	size_t length = 0;

	if (size > MAX_PAYLOAD)
		return DC_STATUS_INVALIDARGS;

	while (1) {
		status = dc_iostream_read (device->iostream, packet, sizeof(packet), &length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (device->base.context, "Failed to receive the packet.");
			return status;
		}

		if (length < 10) {
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
		if (rsp_length != length - 10) {
			ERROR (device->base.context, "Unexpected packet payload length (%u " DC_PRINTF_SIZE ").", rsp_length, length - 10);
			return DC_STATUS_PROTOCOL;
		}

		unsigned int rsp_opcode = packet[1];
		if (rsp_opcode != opcode) {
			WARNING (device->base.context, "Skipping a packet with an unexpected opcode (%02x %02x).", rsp_opcode, opcode);
			continue;
		}

		unsigned int rsp_seqnum = array_uint16_le (packet + 4);
		if (rsp_seqnum != seqnum) {
			WARNING (device->base.context, "Skipping a packet with an unexpected sequence number (%u %u).", rsp_seqnum, seqnum);
			continue;
		}

		if (rsp_length > size) {
			ERROR (device->base.context, "Unexpected packet payload length (%u " DC_PRINTF_SIZE ").", rsp_length, size);
			return DC_STATUS_NOMEMORY;
		}

		memcpy (data, packet + 6, rsp_length);
		if (actual)
			*actual = rsp_length;

		return status;
	}
}

// Send a request on a handle and receive its reply:
//
//   W: HANDLE(3) 01 80 00 PARAMS
//   R: HANDLE(3) 01 80 00 ERRCODE(2) DATA
//
// The reply must echo the handle and the 01 80 00 client id, and carry
// ERR_OK or ERR_CONTINUE; any other error code fails the request. Only the
// DATA is returned, and the error code for the callers that need to tell
// the last page (ERR_OK) from the others (ERR_CONTINUE). The DATA must fill
// the buffer exactly when the caller doesn't ask for its size, and fit in it
// otherwise.
static dc_status_t
suunto_nautic_transfer (suunto_nautic_device_t *device, unsigned char opcode, unsigned char rsp_opcode,
	unsigned int handle, const unsigned char params[], size_t nparams,
	unsigned char data[], size_t size, size_t *actual, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char request[6 + MAX_PARAMS] = {0};
	unsigned char reply[MAX_PAYLOAD] = {0};
	size_t length = 0;
	unsigned int errcode = 0;

	if (nparams > MAX_PARAMS) {
		status = DC_STATUS_INVALIDARGS;
		goto error_exit;
	}

	array_uint24_le_set (request, handle);
	request[3] = 0x01;
	request[4] = 0x80;
	request[5] = 0x00;
	if (nparams)
		memcpy (request + 6, params, nparams);

	unsigned int seqnum = suunto_nautic_seqnum (device);
	status = suunto_nautic_send (device, opcode, seqnum, request, 6 + nparams);
	if (status != DC_STATUS_SUCCESS)
		goto error_exit;

	size_t reply_len = 0;
	status = suunto_nautic_recv (device, rsp_opcode, seqnum, reply, sizeof (reply), &reply_len);
	if (status != DC_STATUS_SUCCESS)
		goto error_exit;

	if (reply_len < 8) {
		ERROR (device->base.context, "Unexpected reply size (" DC_PRINTF_SIZE ").", reply_len);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	unsigned int rsp_handle = array_uint24_le (reply);
	if (rsp_handle != handle) {
		ERROR (device->base.context, "Unexpected reply handle (%06x %06x).", rsp_handle, handle);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	if (memcmp (reply + 3, request + 3, 3) != 0) {
		ERROR (device->base.context, "Unexpected reply client id (%02x %02x %02x).", reply[3], reply[4], reply[5]);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	errcode = array_uint16_le (reply + 6);
	if (errcode != ERR_OK && errcode != ERR_CONTINUE) {
		ERROR (device->base.context, "Unexpected reply error code (%u).", errcode);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	if (actual == NULL && reply_len - 8 != size) {
		ERROR (device->base.context, "Unexpected reply length (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", reply_len - 8, size);
		status = DC_STATUS_PROTOCOL;
		goto error_exit;
	}

	if (reply_len - 8 > size) {
		ERROR (device->base.context, "Unexpected reply length (" DC_PRINTF_SIZE " " DC_PRINTF_SIZE ").", reply_len - 8, size);
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	if (reply_len - 8)
		memcpy (data, reply + 8, reply_len - 8);
	length = reply_len - 8;

error_exit:
	if (actual)
		*actual = length;
	if (errorcode)
		*errorcode = errcode;
	return status;
}

// Release a handle bound by a GET, so the next GET of the same resource binds
// its own id to the slot.
static dc_status_t
suunto_nautic_release (suunto_nautic_device_t *device, unsigned int handle)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (HANDLE_ISSTATIC (handle) || HANDLE_ISINVALID (handle))
		return status;

	status = suunto_nautic_transfer (device, RPC_OP_RELEASE, RPC_OP_RELEASE_REPLY,
		handle, NULL, 0, NULL, 0, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to release handle %06x.", handle);
		return status;
	}

	return status;
}

// Resolve a path to a handle:
//
//   W: 01 80 00 LENGTH PATH(LENGTH)
//   R: HANDLE(3) 01 80 00 ERRCODE(2)
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

	unsigned char reply[8] = {0};
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
	unsigned int errcode = array_uint16_le (reply + 6);
	if (errcode != ERR_OK) {
		ERROR (device->base.context, "Failed to resolve %s (status %u).", path, errcode);
		return DC_STATUS_PROTOCOL;
	}

	if (handle)
		*handle = value;

	return status;
}

// Decompress a Heatshrink (LZSS) stream, with the parameters above, and
// append the result to the output buffer. The input isn't const, because
// heatshrink_decoder_sink() doesn't take a const buffer.
static dc_status_t
suunto_nautic_heatshrink_decompress (dc_context_t *context, unsigned char *input, size_t input_size, dc_buffer_t *output)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char outbuf[HEATSHRINK_INPUT_BUFFER_SIZE];

	heatshrink_decoder *hsd = heatshrink_decoder_alloc (HEATSHRINK_INPUT_BUFFER_SIZE, HEATSHRINK_WINDOW_SZ2, HEATSHRINK_LOOKAHEAD_SZ2);
	if (hsd == NULL) {
		ERROR (context, "Failed to allocate the heatshrink decoder.");
		return DC_STATUS_NOMEMORY;
	}

	size_t sunk_total = 0;
	while (sunk_total < input_size) {
		size_t sunk = 0;
		HSD_sink_res sres = heatshrink_decoder_sink (hsd, input + sunk_total, input_size - sunk_total, &sunk);
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
// still compressed stream, or an error when the watch refuses the stream, a
// chunk is missing or the stream stops before its end marker.
static dc_status_t
suunto_nautic_stream (suunto_nautic_device_t *device, const char *path, dc_buffer_t *buffer)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned int received = 0;
	unsigned int end = 0;

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// The SUBSCRIBE and UNSUBSCRIBE replies carry two more bytes, 00 00.
	unsigned char reply[2] = {0};
	unsigned int errcode = 0;
	status = suunto_nautic_transfer (device, RPC_OP_SUBSCRIBE, RPC_OP_SUBSCRIBE_REPLY,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), NULL, &errcode);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to subscribe to %s.", path);
		goto error_release;
	}

	if (errcode != ERR_OK) {
		ERROR (abstract->context, "Unexpected error code for the subscription to %s (%u).", path, errcode);
		status = DC_STATUS_PROTOCOL;
		goto error_release;
	}

	status = dc_iostream_set_timeout (device->iostream, TIMEOUT_STREAM);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the timeout.");
		goto error_unsubscribe;
	}

	// The watch streams the whole resource without acknowledgements, up to
	// the end marker. Nothing is sent meanwhile, so check for a cancel here.
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
		if (chunk_len != CHUNK_DATA + length + 1 || chunk[CHUNK_DATA + length] != 0) {
			ERROR (abstract->context, "Unexpected chunk length (%u " DC_PRINTF_SIZE ").", length, chunk_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		// A read error on the watch is reported as a chunk at offset
		// 0xFFFFFFFF, with an int16 error code as its data.
		if (offset == 0xFFFFFFFF) {
			if (length != 2) {
				ERROR (abstract->context, "Unexpected read error length (%u).", length);
				status = DC_STATUS_PROTOCOL;
				break;
			}
			ERROR (abstract->context, "Failed to read %s on the watch (%d).", path, (signed short) array_uint16_le (chunk + CHUNK_DATA));
			status = DC_STATUS_PROTOCOL;
			break;
		}

		// The end marker, a chunk without data at the total size, is sent
		// twice, and nothing follows it.
		if (end) {
			if (offset != received || length != 0) {
				ERROR (abstract->context, "Unexpected chunk after the end marker (%u %u).", offset, length);
				status = DC_STATUS_PROTOCOL;
			}
			break;
		}

		if (offset != received) {
			ERROR (abstract->context, "Unexpected chunk offset (%u %u).", offset, received);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (length == 0) {
			end = 1;
			continue;
		}

		if (!dc_buffer_append (buffer, chunk + CHUNK_DATA, length)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}
		received += length;
	}

error_unsubscribe:
	// Without the UNSUBSCRIBE the watch keeps streaming, and the next GET of a
	// stream is refused with status 423.
	rc = suunto_nautic_transfer (device, RPC_OP_UNSUBSCRIBE, RPC_OP_UNSUBSCRIBE_REPLY,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), NULL, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to unsubscribe from %s.", path);
		dc_status_set_error (&status, rc);
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

	unsigned int offset = 0;
	while (1) {
		unsigned char params[7] = {0x01, PARAM_INT32, 0x00};
		array_uint32_le_set (params + 3, offset);

		unsigned char page[MAX_PAYLOAD] = {0};
		size_t page_len = 0;
		unsigned int errcode = 0;
		status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
			handle, params, sizeof (params), page, sizeof (page), &page_len, &errcode);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to fetch %s (offset %u).", path, offset);
			break;
		}

		if (page_len < PAGE_HEADER) {
			ERROR (abstract->context, "Unexpected page size (" DC_PRINTF_SIZE ").", page_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		// An empty page wouldn't advance the offset, so the next request
		// would be the same one again. Only the last page may be empty.
		unsigned int length = array_uint16_le (page + PAGE_LENGTH);
		if ((length == 0 && errcode != ERR_OK) || PAGE_HEADER + length != page_len) {
			ERROR (abstract->context, "Unexpected page length (%u " DC_PRINTF_SIZE ").", length, page_len);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		if (!dc_buffer_append (buffer, page + PAGE_HEADER, length)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}
		offset += length;

		if (errcode == ERR_OK)
			break;
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
	unsigned int errcode = 0;
	status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
		handle, suunto_nautic_noparams, sizeof (suunto_nautic_noparams), reply, sizeof (reply), &reply_len, &errcode);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to fetch %s.", path);
		goto error_release;
	}

	if (errcode != ERR_OK) {
		ERROR (abstract->context, "Unexpected error code for %s (%u).", path, errcode);
		status = DC_STATUS_PROTOCOL;
		goto error_release;
	}

	if (!dc_buffer_append (buffer, reply, reply_len)) {
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

	unsigned int handle = 0;
	status = suunto_nautic_get_handle (device, path, &handle);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// The first page is requested without the StartAfterId, which is
	// optional, as the Suunto app does. Per the Movesense logbook API, a
	// StartAfterId that isn't the id of an entry, such as 0, gets a 404.
	unsigned char params[7] = {0x00, PARAM_UINT32, 0x00};
	size_t nparams = 1;
	while (1) {
		unsigned char data[MAX_PAYLOAD] = {0};
		size_t size = 0;
		unsigned int errcode = 0;
		status = suunto_nautic_transfer (device, RPC_OP_FETCH, RPC_OP_DATA,
			handle, params, nparams, data, sizeof (data), &size, &errcode);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to fetch %s.", path);
			break;
		}

		if (size < ENTRIES_STRUCT + 8) {
			ERROR (abstract->context, "Unexpected entries size (" DC_PRINTF_SIZE ").", size);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		// The page holds exactly the records, after the array header.
		unsigned int count = array_uint16_le (data + ENTRIES_STRUCT + ENTRIES_COUNT);
		unsigned int start = array_uint32_le (data + ENTRIES_STRUCT + ENTRIES_RECORDS);
		if (start < 8 || start > size - ENTRIES_STRUCT ||
			size - ENTRIES_STRUCT - start != count * ENTRY_SIZE) {
			ERROR (abstract->context, "Unexpected number of entries (%u %u " DC_PRINTF_SIZE ").", count, start, size);
			status = DC_STATUS_PROTOCOL;
			break;
		}

		const unsigned char *records = data + ENTRIES_STRUCT + start;
		if (!dc_buffer_append (entries, records, count * ENTRY_SIZE)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}

		if (errcode == ERR_OK)
			break;

		if (count == 0) {
			ERROR (abstract->context, "No entries with more pages pending.");
			status = DC_STATUS_PROTOCOL;
			break;
		}

		// The next page starts after the last id received.
		params[0] = 0x01;
		memcpy (params + 3, records + (count - 1) * ENTRY_SIZE + ENTRY_START, 4);
		nparams = sizeof (params);
	}

	dc_status_t rc = suunto_nautic_release (device, handle);
	if (rc != DC_STATUS_SUCCESS)
		dc_status_set_error (&status, rc);

	return status;
}

// Download one dive as SUMMARY_SIZE(4) SUMMARY PROFILE: the /Summary, with
// the fields that are not in the profile (gradient factors, gas mixes), like
// a dive header, and then the decompressed /Data stream. Both are SBEM0103
// containers, which don't store their own size, hence the size in front.
// The listed size is the compressed /Data plus the /Summary data bytes,
// exactly, and a mismatch fails the download. Checking the content is left
// to the parser.
static dc_status_t
suunto_nautic_download (suunto_nautic_device_t *device, unsigned int id, unsigned int listed, dc_buffer_t *buffer)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;

	char data_path[64], summary_path[64];
	snprintf (data_path, sizeof (data_path), "/Logbook/byId/%u/Data", id);
	snprintf (summary_path, sizeof (summary_path), "/Logbook/byId/%u/Summary", id);

	dc_buffer_t *compressed = dc_buffer_new (listed);
	if (compressed == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// The size is filled in once the /Summary is complete.
	unsigned char header[4] = {0};
	if (!dc_buffer_append (buffer, header, sizeof (header))) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_free;
	}

	// The /Summary goes first: it is much smaller than the /Data, so an
	// error there doesn't come after the long download of the profile.
	status = suunto_nautic_fetch_pages (device, summary_path, buffer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to fetch the summary of dive %u.", id);
		goto error_free;
	}

	size_t summary_size = dc_buffer_get_size (buffer) - sizeof (header);
	array_uint32_le_set (dc_buffer_get_data (buffer), summary_size);

	status = suunto_nautic_stream (device, data_path, compressed);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to download the profile of dive %u.", id);
		goto error_free;
	}

	size_t data_size = dc_buffer_get_size (compressed);
	if (data_size + summary_size != listed) {
		ERROR (abstract->context, "Unexpected size of dive %u (" DC_PRINTF_SIZE " + " DC_PRINTF_SIZE " bytes, listed %u).",
			id, data_size, summary_size, listed);
		status = DC_STATUS_PROTOCOL;
		goto error_free;
	}

	status = suunto_nautic_heatshrink_decompress (abstract->context,
		dc_buffer_get_data (compressed), dc_buffer_get_size (compressed), buffer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to decompress dive %u.", id);
		goto error_free;
	}

error_free:
	dc_buffer_free (compressed);
	return status;
}

// Return the string at the given index of the /Info strings, or NULL when its
// offset is outside the data or it isn't NUL-terminated there.
static const char *
suunto_nautic_info_string (const unsigned char data[], size_t size, unsigned int index)
{
	if (INFO_STRUCT + (index + 1) * 4 > size)
		return NULL;

	unsigned int offset = array_uint32_le (data + INFO_STRUCT + index * 4);
	if (offset >= size - INFO_STRUCT)
		return NULL;

	const unsigned char *string = data + INFO_STRUCT + offset;
	if (memchr (string, 0, size - INFO_STRUCT - offset) == NULL)
		return NULL;

	return (const char *) string;
}

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
		goto error_free;
	}

	const unsigned char *data = dc_buffer_get_data (info);
	size_t size = dc_buffer_get_size (info);

	if (size < INFO_STRUCT || array_uint16_le (data) != INFO_TYPE) {
		ERROR (abstract->context, "Unexpected device info type.");
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	const char *serial = suunto_nautic_info_string (data, size, INFO_SERIAL);
	unsigned char number[4] = {0};
	if (serial == NULL || strlen (serial) != 12 ||
		array_convert_hex2bin ((const unsigned char *) serial + 4, 8, number, sizeof (number)) != 0) {
		ERROR (abstract->context, "Unexpected serial in the device info.");
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	const char *firmware = suunto_nautic_info_string (data, size, INFO_FIRMWARE);
	unsigned int major = 0, minor = 0, patch = 0;
	if (firmware == NULL || sscanf (firmware, "%u.%u.%u", &major, &minor, &patch) != 3) {
		ERROR (abstract->context, "Unexpected firmware in the device info.");
		status = DC_STATUS_DATAFORMAT;
		goto error_free;
	}

	dc_event_devinfo_t devinfo;
	memset (&devinfo, 0, sizeof (devinfo));
	devinfo.firmware = (major << 16) | (minor << 8) | patch;
	devinfo.serial = array_uint32_be (number);

	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

error_free:
	dc_buffer_free (info);
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

static dc_status_t
suunto_nautic_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	status = suunto_nautic_devinfo (device);
	if (status != DC_STATUS_SUCCESS)
		return status;

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

	// The entries are oldest first, and the dives are downloaded newest
	// first, up to the one that matches the fingerprint. Count those.
	unsigned int ndives = 0;
	for (unsigned int i = count; i > 0; i--) {
		const unsigned char *record = records + (i - 1) * ENTRY_SIZE;
		if (memcmp (record + ENTRY_START, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;
		ndives++;
	}

	progress.current = 1 * NSTEPS;
	progress.maximum = (ndives + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	for (unsigned int i = 0; i < ndives; i++) {
		const unsigned char *record = records + (count - 1 - i) * ENTRY_SIZE;
		unsigned int id = array_uint32_le (record + ENTRY_START);
		unsigned int listed = array_uint32_le (record + ENTRY_LENGTH);

		// The size is optional in the API, but no watch has been seen to list
		// an entry without one, or with one that needs more than 32 bits.
		if (record[ENTRY_HASSIZE] != 1 || array_uint32_le (record + ENTRY_LENGTH + 4) != 0) {
			ERROR (abstract->context, "Unexpected size of dive %u.", id);
			status = DC_STATUS_DATAFORMAT;
			break;
		}

		dc_buffer_clear (buffer);
		if (!dc_buffer_reserve (buffer, listed)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			status = DC_STATUS_NOMEMORY;
			break;
		}

		status = suunto_nautic_download (device, id, listed, buffer);
		if (status != DC_STATUS_SUCCESS) {
			// Stop here: the fingerprint of a newer dive would mark this one
			// as downloaded.
			ERROR (abstract->context, "Failed to download dive %u.", id);
			break;
		}

		progress.current = (i + 2) * NSTEPS;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		// The fingerprint is the id, as it is stored in the record.
		if (callback && !callback (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), record + ENTRY_START, sizeof (device->fingerprint), userdata))
			break;
	}

error_free:
	dc_buffer_free (buffer);
	dc_buffer_free (entries);
	return status;
}
