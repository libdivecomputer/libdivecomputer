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

// See suunto_nautic.h for a description of the transport and format.

#define RPC_OP_GET           0x0A
#define RPC_OP_STREAM_FETCH1 0x0B
#define RPC_OP_FETCH         0x0D
#define RPC_OP_STREAM_END    0x09 // watch -> host: the stream is closed (answers STREAM_STOP)
#define RPC_OP_STREAM_FETCH2 0x10
#define RPC_OP_STREAM_STOP   0x11 // host -> watch: close the stream and release its handle
#define RPC_OP_DATA          0x05

#define RPC_HEADER_SIZE 10 // magic(1) + opcode(1) + sublen(2) + seq(2) + 0x01 + 0x80 + 0x00 + pathlen(1)
#define RPC_CRC_SIZE     4

// Offset of the 3-byte session handle inside an ACK (0x02) or DATA (0x05)
// frame: magic(1) + opcode(1) + sublen(2) + msgid(2).
#define RPC_HANDLE_OFFSET 6
// Offset of the 16-bit LE HTTP-like status inside a DATA (0x05) frame:
// magic(1) + opcode(1) + sublen(2) + msgid(2) + handle(3) + flags(3).
#define RPC_STATUS_OFFSET 12
#define RPC_STATUS_OK       200
#define RPC_STATUS_CONTINUE 100 // more pages follow (paginated fetch)

#define MAX_PATH    240
#define MAX_PACKET  512

// Dive IDs are UNIX timestamps. /Logbook/Entries embeds them as 4-aligned
// little-endian uint32 values in a small SBEM payload among handle/flag/
// count/CRC fields; filtering to a plausible timestamp window (2017 .. 2036)
// isolates them. Must scan 4-aligned -- the IDs are packed adjacently, so an
// unaligned read straddling two can invent a phantom dive.
#define DIVE_ID_MIN 1500000000u
#define DIVE_ID_MAX 2100000000u

// Each entry stores start then end timestamp adjacently; an end is always
// within a day of its start. Used to pair (start, end) so a dive's end isn't
// listed as a second dive.
#define DIVE_ENTRY_MAX_PAIR_GAP 86400u

// Number of PMT-style chunks to accept before giving up. This is a safety
// cap, not a protocol constant: the stream is collected until an inter-frame
// silence, then closed explicitly with STREAM_STOP (see the teardown in
// suunto_nautic_device_stream_fetch).
#define MAX_CHUNKS 4096

// Safety cap on paginated-fetch pages (a Summary is a handful of pages).
#define MAX_PAGES 64

// How many times to (re)issue a dive's stream fetch. The watch can refuse the
// first attempt with status 423 (Locked) right after the previous dive; a short
// backoff between attempts clears it.
#define SUUNTO_NAUTIC_DOWNLOAD_RETRIES 4

// The Suunto "MDS" chunk header wrapping each compressed block: 28 bytes,
// with the true payload size as a u16 LE at offset 20 and the compressed
// payload starting at offset 28.
#define MDS_HEADER_SIZE     28
#define MDS_CHUNK_SIZE_OFFSET 20

// Heatshrink (LZSS) parameters used by the Nautic/Ocean's MDS stream.
#define HEATSHRINK_WINDOW_SZ2    7
#define HEATSHRINK_LOOKAHEAD_SZ2 5
#define HEATSHRINK_INPUT_BUFFER_SIZE 256

static const unsigned char SBEM_MAGIC[8] = {'S','B','E','M','0','1','0','3'};

typedef struct suunto_nautic_device_t {
	dc_device_t base;
	dc_iostream_t *iostream; // HDLC-framed
	unsigned int sequence;
	// The dive ID (a UNIX timestamp, see suunto_nautic_device_foreach) of
	// the most recently downloaded dive, little-endian, as returned via
	// dc_dive_callback_t's fingerprint parameter. All-zero means "no
	// fingerprint set" (a real dive ID is never 0 -- that would be a
	// 1970 timestamp), matching every other driver's convention.
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

/*
 * The "EVA" handshake is the Whiteboard protocol's Hello message (message
 * type 0x12). The payload carries a SuuntoSerial identity, a fixed
 * protocol-version block, a capability-flags byte and a trailing CRC32.
 * The identity has no cryptographic tie to a specific phone, and the watch
 * has only been confirmed to answer the captured template, so it is sent
 * verbatim.
 */
static const unsigned char suunto_nautic_eva_handshake[] = {
	0xA5, 0x12, 0x20, 0x00, 0x00, 0x00, 0x09, 0x09, 0x20, 0x16, 0x45, 0x56,
	0x41, 0x10, 0x04, 0x41, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x04, 0x01, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x63, 0x1B, 0x47, 0x1B
};

#define EVA_HANDSHAKE_SIZE (sizeof (suunto_nautic_eva_handshake))

/*
 * Stream-fetch trigger tails, captured verbatim. The sequence-number field
 * (bytes 4-5) is the watch's session handle for this transfer plus 1
 * (FETCH1) or plus 2 (FETCH2), read from the ACK to the preceding GET
 * request; see suunto_nautic_device_download(). The remaining tail bytes
 * are replayed literally.
 */
static const unsigned char suunto_nautic_fetch1_tail[] = {
	0x00, 0x24, 0x12, 0x01, 0x80, 0x00
};
static const unsigned char suunto_nautic_fetch2_tail[] = {
	0x00, 0x24, 0x0E, 0x01, 0x80, 0x00, 0x00
};
// Bare STREAM_FETCH1 on the stream handle (0x240E), no trailing byte. Sent once
// per dive right after the /Data stream is closed -- as part of the trailing
// /Summary fetch, exactly as the official app does -- to re-arm the stream
// channel. Without it the watch replays the just-downloaded dive's buffered
// data for the next dive's /Data request.
static const unsigned char suunto_nautic_stream_rearm_tail[] = {
	0x00, 0x24, 0x0E, 0x01, 0x80, 0x00
};

// Build a generic path-addressed GET request for an arbitrary endpoint.
static dc_status_t
suunto_nautic_build_get (unsigned char packet[], unsigned int size, unsigned int *out_len, unsigned int seq, const char *path)
{
	size_t pathlen = strlen (path);
	if (pathlen == 0 || pathlen > MAX_PATH)
		return DC_STATUS_INVALIDARGS;

	unsigned int len = RPC_HEADER_SIZE + (unsigned int) pathlen + RPC_CRC_SIZE;
	if (len > size)
		return DC_STATUS_INVALIDARGS;

	unsigned int sublen = (unsigned int) pathlen + 4;

	packet[0] = 0xA5;
	packet[1] = RPC_OP_GET;
	array_uint16_le_set (packet + 2, (unsigned short) sublen);
	array_uint16_le_set (packet + 4, (unsigned short) seq);
	packet[6] = 0x01;
	packet[7] = 0x80;
	packet[8] = 0x00;
	packet[9] = (unsigned char) pathlen;
	memcpy (packet + 10, path, pathlen);

	unsigned int crc = checksum_crc32r (packet, RPC_HEADER_SIZE + (unsigned int) pathlen);
	array_uint32_le_set (packet + RPC_HEADER_SIZE + (unsigned int) pathlen, crc);

	*out_len = len;
	return DC_STATUS_SUCCESS;
}

// Build a stream-fetch trigger frame. Only the opcode and sequence number
// are derived; the tail is a literal replay (see the caveats above the
// suunto_nautic_fetch{1,2}_tail tables).
static dc_status_t
suunto_nautic_build_stream_fetch (unsigned char packet[], unsigned int size, unsigned int *out_len,
	unsigned int seq, unsigned char opcode, const unsigned char tail[], unsigned int tail_size)
{
	unsigned int len = RPC_HEADER_SIZE - 4 + tail_size + RPC_CRC_SIZE; // magic+opcode+sublen+seq (6) + tail + crc
	if (len > size)
		return DC_STATUS_INVALIDARGS;

	packet[0] = 0xA5;
	packet[1] = opcode;
	array_uint16_le_set (packet + 2, (unsigned short) tail_size);
	array_uint16_le_set (packet + 4, (unsigned short) seq);
	memcpy (packet + 6, tail, tail_size);

	unsigned int crc = checksum_crc32r (packet, 6 + tail_size);
	array_uint32_le_set (packet + 6 + tail_size, crc);

	*out_len = len;
	return DC_STATUS_SUCCESS;
}

// Build the "short" fetch (opcode 0x0D) the official app uses to read a
// small whole resource in one shot, e.g. /Logbook/Entries. Payload is
// [seq:2 LE][handle:3][01 80 00 00] -- the trailing 01 80 00 00 is the
// no-range form. NOT the ranged fetch used for large paginated resources
// (Summary/Data), whose payload ends 01 80 00 01 06 00 [offset:4]; sending
// that ranged form to /Logbook/Entries makes the watch reject it with a
// 400 Bad Request.
static dc_status_t
suunto_nautic_build_short_fetch (unsigned char packet[], unsigned int size, unsigned int *out_len,
	unsigned int seq, const unsigned char handle[3])
{
	static const unsigned char tail[] = { 0x01, 0x80, 0x00, 0x00 };
	unsigned int payload = 3 + (unsigned int) sizeof (tail); // handle(3) + tail
	unsigned int len = 4 + 2 + payload + RPC_CRC_SIZE;        // magic+opcode+sublen(4) + seq(2) + payload + crc
	if (len > size)
		return DC_STATUS_INVALIDARGS;

	packet[0] = 0xA5;
	packet[1] = RPC_OP_FETCH;
	// sublen counts seq(2)+payload minus 2, i.e. payload itself.
	array_uint16_le_set (packet + 2, (unsigned short) payload);
	array_uint16_le_set (packet + 4, (unsigned short) seq);
	memcpy (packet + 6, handle, 3);
	memcpy (packet + 9, tail, sizeof (tail));

	unsigned int crc = checksum_crc32r (packet, 6 + payload);
	array_uint32_le_set (packet + 6 + payload, crc);

	*out_len = len;
	return DC_STATUS_SUCCESS;
}

// Build a paginated fetch (opcode 0x0D) for a resource the watch returns
// across multiple pages, e.g. /Logbook/byId/<id>/Summary. Payload is
// [seq:2 LE][handle:3][01 80 00 01 06 00][offset:4 LE] -- the ranged form.
// The watch answers each page with HTTP status 100 (more pages) or 200
// (last page).
static dc_status_t
suunto_nautic_build_paginated_fetch (unsigned char packet[], unsigned int size, unsigned int *out_len,
	unsigned int seq, const unsigned char handle[3], unsigned int offset)
{
	static const unsigned char flags[] = { 0x01, 0x80, 0x00, 0x01, 0x06, 0x00 };
	unsigned int payload = 3 + (unsigned int) sizeof (flags) + 4; // handle(3) + flags(6) + offset(4)
	unsigned int len = 4 + 2 + payload + RPC_CRC_SIZE;
	if (len > size)
		return DC_STATUS_INVALIDARGS;

	packet[0] = 0xA5;
	packet[1] = RPC_OP_FETCH;
	array_uint16_le_set (packet + 2, (unsigned short) payload);
	array_uint16_le_set (packet + 4, (unsigned short) seq);
	memcpy (packet + 6, handle, 3);
	memcpy (packet + 9, flags, sizeof (flags));
	array_uint32_le_set (packet + 9 + (unsigned int) sizeof (flags), offset);

	unsigned int crc = checksum_crc32r (packet, 6 + payload);
	array_uint32_le_set (packet + 6 + payload, crc);

	*out_len = len;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_transfer (suunto_nautic_device_t *device, const unsigned char req[], unsigned int rsize, dc_buffer_t *response)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	status = dc_iostream_write (device->iostream, req, rsize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the RPC request.");
		return status;
	}

	if (response) {
		unsigned char packet[MAX_PACKET] = {0};
		size_t len = 0;
		status = dc_iostream_read (device->iostream, packet, sizeof (packet), &len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the RPC response.");
			return status;
		}

		HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "RPC RSP", packet, len);

		dc_buffer_clear (response);
		if (!dc_buffer_append (response, packet, len)) {
			ERROR (abstract->context, "Failed to allocate memory.");
			return DC_STATUS_NOMEMORY;
		}
	}

	return DC_STATUS_SUCCESS;
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

	device->sequence = 1;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	status = dc_hdlc_open (&device->iostream, context, iostream, 244, 244);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the HDLC layer.");
		goto error_free;
	}

	status = dc_iostream_set_timeout (device->iostream, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Best-effort EVA handshake. The response content can't be validated
	// (its format isn't understood), so only the I/O round-trip is required.
	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "EVA REQ", suunto_nautic_eva_handshake, EVA_HANDSHAKE_SIZE);

	status = dc_iostream_write (device->iostream, suunto_nautic_eva_handshake, EVA_HANDSHAKE_SIZE, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the EVA handshake.");
		goto error_close;
	}

	unsigned char handshake_rsp[MAX_PACKET] = {0};
	size_t handshake_len = 0;
	status = dc_iostream_read (device->iostream, handshake_rsp, sizeof (handshake_rsp), &handshake_len);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to receive the EVA handshake response. The device may not "
			"support this protocol, or the handshake payload may need updating "
			"(see suunto_nautic.h).");
		goto error_close;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "EVA RSP", handshake_rsp, handshake_len);

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
suunto_nautic_device_request (dc_device_t *abstract, const char *path, dc_buffer_t *response)
{
	if (abstract == NULL || abstract->vtable->type != DC_FAMILY_SUUNTO_NAUTIC || path == NULL)
		return DC_STATUS_INVALIDARGS;

	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	unsigned char packet[RPC_HEADER_SIZE + MAX_PATH + RPC_CRC_SIZE];
	unsigned int len = 0;
	dc_status_t status = suunto_nautic_build_get (packet, sizeof (packet), &len, device->sequence, path);
	if (status != DC_STATUS_SUCCESS)
		return status;
	device->sequence++;

	return suunto_nautic_transfer (device, packet, len, response);
}

// Decompress a Heatshrink (LZSS) stream, per the parameters documented
// above. Verified byte-for-byte against a reference implementation using
// real captured data (see suunto_nautic.h).
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

// Append one MDS chunk frame's sub-payload (opcode 0x01) to `raw`: the 28-byte
// MDS header is stripped and the true payload length is a u16 at
// MDS_CHUNK_SIZE_OFFSET. A short or inconsistent frame is skipped with a warning
// rather than failing the transfer; only an allocation failure is fatal.
static dc_status_t
suunto_nautic_append_chunk (dc_context_t *context, dc_buffer_t *raw, const unsigned char *packet, size_t len)
{
	if (len < MDS_HEADER_SIZE) {
		WARNING (context, "MDS chunk shorter than the header (" DC_PRINTF_SIZE ").", len);
		return DC_STATUS_SUCCESS;
	}

	unsigned int chunk_size = array_uint16_le (packet + MDS_CHUNK_SIZE_OFFSET);
	if (MDS_HEADER_SIZE + chunk_size > len) {
		WARNING (context, "MDS chunk size (%u) exceeds the frame (" DC_PRINTF_SIZE ").", chunk_size, len);
		return DC_STATUS_SUCCESS;
	}

	if (!dc_buffer_append (raw, packet + MDS_HEADER_SIZE, chunk_size)) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	return DC_STATUS_SUCCESS;
}

// Performs the GET -> ACK(watch magic) -> FETCH1 -> FETCH2 -> stream-collect ->
// STREAM_STOP sequence used to pull a large paginated resource (dive data).
// Returns the raw, MDS-chunk-stripped, still-Heatshrink-compressed bytes. Small
// listing endpoints use suunto_nautic_device_short_fetch() instead.
static dc_status_t
suunto_nautic_device_stream_fetch (dc_device_t *abstract, const char *path, dc_buffer_t *raw)
{
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;

	// 1. Request the resource. The watch's ACK carries a "Watch Magic"
	// session id (little-endian UInt32 at offset 5) that authorizes this
	// transfer; the two stream-fetch triggers below use Watch_Magic+1/+2.
	dc_buffer_t *ack = dc_buffer_new (0);
	if (ack == NULL)
		return DC_STATUS_NOMEMORY;

	status = suunto_nautic_device_request (abstract, path, ack);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "Failed to request %s.", path);
		return status;
	}

	const unsigned char *ack_data = dc_buffer_get_data (ack);
	size_t ack_size = dc_buffer_get_size (ack);
	if (ack_size < 9) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "ACK response too short to contain the watch magic (" DC_PRINTF_SIZE ").", ack_size);
		return DC_STATUS_DATAFORMAT;
	}
	unsigned int watch_magic = array_uint32_le (ack_data + 5);
	dc_buffer_free (ack);

	// 2. Trigger the stream using Watch_Magic+1/+2.
	unsigned char fetch[32];
	unsigned int fetch_len = 0;

	status = suunto_nautic_build_stream_fetch (fetch, sizeof (fetch), &fetch_len, watch_magic + 1,
		RPC_OP_STREAM_FETCH1, suunto_nautic_fetch1_tail, sizeof (suunto_nautic_fetch1_tail));
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = dc_iostream_write (device->iostream, fetch, fetch_len, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the first stream-fetch trigger.");
		return status;
	}

	status = suunto_nautic_build_stream_fetch (fetch, sizeof (fetch), &fetch_len, watch_magic + 2,
		RPC_OP_STREAM_FETCH2, suunto_nautic_fetch2_tail, sizeof (suunto_nautic_fetch2_tail));
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = dc_iostream_write (device->iostream, fetch, fetch_len, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the second stream-fetch trigger.");
		return status;
	}

	// 3. Capture the MDS chunk frames (opcode 0x01) and pull out each
	// one's true sub-payload: the MDS header is 28 bytes, and the payload
	// size is a little-endian u16 at offset 20-21 (see the MDS_HEADER_SIZE
	// comment above). For a compressed endpoint, the concatenation of
	// these sub-payloads across all chunks is one continuous Heatshrink
	// stream — chunk boundaries are purely a BLE/transport artifact, not
	// boundaries in the compressed data.
	//
	// The watch is not ACKed per chunk: once FETCH2 is sent it streams the
	// entire response continuously, and the host buffers until an inter-frame
	// silence (the watch only emits its own STREAM_END frame in response to a
	// STREAM_STOP, which we send once collection has gone quiet -- see the
	// teardown below). The official app's inter-chunk gap tops out near 2s, so
	// keep a comfortable margin here to avoid cutting a slow link short.
	status = dc_iostream_set_timeout (device->iostream, 4000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the stream timeout.");
		return status;
	}

	for (unsigned int i = 0; i < MAX_CHUNKS; i++) {
		unsigned char packet[MAX_PACKET] = {0};
		size_t len = 0;
		status = dc_iostream_read (device->iostream, packet, sizeof (packet), &len);
		if (status != DC_STATUS_SUCCESS) {
			if (status == DC_STATUS_TIMEOUT)
				break;
			ERROR (abstract->context, "Failed to receive a stream chunk.");
			return status;
		}

		if (len == 0)
			break;

		if (len >= 2 && packet[0] == 0xA5 && packet[1] == RPC_OP_STREAM_END)
			break;

		// The watch can refuse the stream with a short status frame (op 0x08)
		// instead of streaming MDS chunks -- notably HTTP-style status 423
		// (Locked) when this dive's GET lands before the previous dive's stream
		// session has been torn down (back-to-back downloads share one BLE
		// link). A status-200 op 0x08 is the normal stream-start ack and is
		// ignored here; a non-200 one is surfaced so the caller can back off
		// and retry rather than silently collecting nothing and reporting the
		// dive as empty/aborted.
		if (len >= RPC_STATUS_OFFSET + 2 && packet[0] == 0xA5 && packet[1] == 0x08) {
			unsigned int frame_status = array_uint16_le (packet + RPC_STATUS_OFFSET);
			if (frame_status != RPC_STATUS_OK) {
				ERROR (abstract->context, "Watch refused the stream for %s (status %u).", path, frame_status);
				return DC_STATUS_PROTOCOL;
			}
		}

		if (len >= 2 && packet[0] == 0xA5 && packet[1] == 0x01) {
			status = suunto_nautic_append_chunk (abstract->context, raw, packet, len);
			if (status != DC_STATUS_SUCCESS)
				return status;
		}
	}

	// 4. Close the stream. Collection above stops on an inter-frame silence,
	// but the watch still considers the stream open and holds its handle
	// (0x240E) -- it even retransmits its last chunk waiting to be told we are
	// done. The official app ends every dive stream with a STREAM_STOP (opcode
	// 0x11, Watch_Magic+3, same tail as FETCH2); the watch answers with a
	// STREAM_END (0x09) frame and releases the handle. Skipping this is
	// harmless for a one-shot download but makes the *next* dive's GET on the
	// same BLE link fail with 423 Locked, because the handle is still busy.
	// Best-effort: we already have the payload, so a failed teardown only
	// warns.
	unsigned char stop[32];
	unsigned int stop_len = 0;
	if (suunto_nautic_build_stream_fetch (stop, sizeof (stop), &stop_len, watch_magic + 3,
			RPC_OP_STREAM_STOP, suunto_nautic_fetch2_tail, sizeof (suunto_nautic_fetch2_tail)) == DC_STATUS_SUCCESS &&
		dc_iostream_write (device->iostream, stop, stop_len, NULL) == DC_STATUS_SUCCESS) {
		// Drain until the STREAM_END ack (or a short silence): a few trailing
		// chunks can still arrive between our STOP and the watch acting on it,
		// but the ack lands well within 100 ms in the reference capture.
		dc_iostream_set_timeout (device->iostream, 2000);
		for (unsigned int i = 0; i < 16; i++) {
			unsigned char packet[MAX_PACKET] = {0};
			size_t len = 0;
			if (dc_iostream_read (device->iostream, packet, sizeof (packet), &len) != DC_STATUS_SUCCESS || len == 0)
				break;
			if (len >= 2 && packet[0] == 0xA5 && packet[1] == RPC_OP_STREAM_END)
				break;
			// A last chunk or two can still be in flight; keep them.
			if (len >= 2 && packet[0] == 0xA5 && packet[1] == 0x01)
				suunto_nautic_append_chunk (abstract->context, raw, packet, len);
		}
	} else {
		WARNING (abstract->context, "Failed to send the stream-stop for %s; the next dive may be refused with 423.", path);
	}

	return DC_STATUS_SUCCESS;
}

// A well-formed RPC frame from the watch starts with the 0xA5 magic and is long
// enough to carry an opcode. Anything shorter is line noise or a truncated read.
static int
suunto_nautic_frame_wellformed (const unsigned char *packet, size_t len)
{
	return len >= 2 && packet[0] == 0xA5;
}

// The DATA (0x05) frame a fetch is waiting for: our opcode, carrying the 3-byte
// session handle this fetch was issued against.
static int
suunto_nautic_frame_is_our_data (const unsigned char *packet, size_t len, const unsigned char handle[3])
{
	return len >= RPC_HANDLE_OFFSET + 3 &&
		packet[0] == 0xA5 && packet[1] == RPC_OP_DATA &&
		memcmp (packet + RPC_HANDLE_OFFSET, handle, 3) == 0;
}

// A single BLE link is shared: another client (most often the official Suunto
// app holding a live logbook subscription) can flood it with its own frames --
// notably Whiteboard 0x07 subscribe-result traffic on a different handle. Those
// are well-formed frames that just aren't ours, so skip them freely rather than
// treating them as a data error. This cap only guards against an unbounded loop;
// in practice the read below times out first (a clean DC_STATUS_TIMEOUT) when
// our DATA frame never gets a turn on the contended link.
#define MAX_FOREIGN_SKIPS 256
// A tight cap on genuinely malformed/truncated frames: those do signal a real
// data-format problem, so give up quickly with DC_STATUS_DATAFORMAT.
#define MAX_MALFORMED_SKIPS 8

// Fetch a resource the watch paginates (e.g. /Logbook/byId/<id>/Summary):
// GET -> ACK(handle) -> repeated ranged 0x0D fetch, looping while the page
// status is 100 (more pages) until 200 (last page), stripping the 10-byte
// REST sub-header from each page and accumulating the raw (uncompressed)
// SBEM bytes into `response`. The per-page REST sub-header (at packet+4) is
// [msgid:2][handle:3][flags:3][status:2 LE].
static dc_status_t
suunto_nautic_device_paginated_fetch (dc_device_t *abstract, const char *path, dc_buffer_t *response)
{
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;

	dc_buffer_t *ack = dc_buffer_new (0);
	if (ack == NULL)
		return DC_STATUS_NOMEMORY;

	status = suunto_nautic_device_request (abstract, path, ack);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "Failed to request %s.", path);
		return status;
	}

	const unsigned char *ack_data = dc_buffer_get_data (ack);
	size_t ack_size = dc_buffer_get_size (ack);
	if (ack_size < RPC_HANDLE_OFFSET + 3) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "ACK too short for a handle (" DC_PRINTF_SIZE ").", ack_size);
		return DC_STATUS_DATAFORMAT;
	}
	unsigned char handle[3];
	memcpy (handle, ack_data + RPC_HANDLE_OFFSET, sizeof (handle));
	dc_buffer_free (ack);

	// Re-arm the stream channel (handle 0x240E) before paging this resource.
	// The watch keeps the previous dive's /Data buffered on that handle and
	// will replay it for the next dive unless a bare STREAM_FETCH1 resets it;
	// the official app sends exactly this 0x0B alongside the trailing /Summary
	// fetch. Best-effort -- a failure here only risks the next dive, and the
	// per-dive retry in suunto_nautic_device_download is the backstop.
	unsigned char rearm[32];
	unsigned int rearm_len = 0;
	if (suunto_nautic_build_stream_fetch (rearm, sizeof (rearm), &rearm_len, device->sequence,
			RPC_OP_STREAM_FETCH1, suunto_nautic_stream_rearm_tail, sizeof (suunto_nautic_stream_rearm_tail)) == DC_STATUS_SUCCESS) {
		device->sequence++;
		if (dc_iostream_write (device->iostream, rearm, rearm_len, NULL) == DC_STATUS_SUCCESS) {
			dc_iostream_set_timeout (device->iostream, 2000);
			unsigned char ackpkt[MAX_PACKET] = {0};
			size_t acklen = 0;
			dc_iostream_read (device->iostream, ackpkt, sizeof (ackpkt), &acklen); // 0x03 ack, ignored
		}
	}

	dc_buffer_clear (response);
	unsigned int offset = 0;
	const unsigned int header = 4 + 10; // A5 05 sublen(2) + 10-byte REST sub-header

	for (unsigned int page = 0; page < MAX_PAGES; page++) {
		unsigned char fetch[32];
		unsigned int fetch_len = 0;
		status = suunto_nautic_build_paginated_fetch (fetch, sizeof (fetch), &fetch_len,
			device->sequence, handle, offset);
		if (status != DC_STATUS_SUCCESS)
			return status;
		device->sequence++;

		status = dc_iostream_write (device->iostream, fetch, fetch_len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send paginated fetch (offset=%u) for %s.", offset, path);
			return status;
		}

		// Read the page, skipping unsolicited/interleaved frames: the watch
		// multiplexes other resources (device info, analytics, a re-sent Hello)
		// on the same link, and another client (the Suunto app's logbook
		// subscription) can flood it with foreign frames. Accept only a DATA
		// (0x05) frame whose handle matches this fetch; skip anything else so a
		// foreign frame isn't spliced into the paginated stream.
		unsigned char packet[MAX_PACKET] = {0};
		size_t len = 0;
		unsigned int foreign_skips = 0;
		unsigned int malformed_skips = 0;
		for (;;) {
			status = dc_iostream_read (device->iostream, packet, sizeof (packet), &len);
			if (status != DC_STATUS_SUCCESS) {
				// A contended link (another client holding a subscription) means
				// our DATA never gets a turn and this read times out. Surface it
				// as-is (DC_STATUS_TIMEOUT) rather than a misleading DATAFORMAT.
				ERROR (abstract->context, "Failed to receive page %u for %s.", page, path);
				return status;
			}
			HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "PFETCH RSP", packet, len);
			if (suunto_nautic_frame_is_our_data (packet, len, handle))
				break;
			if (suunto_nautic_frame_wellformed (packet, len)) {
				if (++foreign_skips >= MAX_FOREIGN_SKIPS) {
					ERROR (abstract->context, "Link saturated by another client while paging %s; giving up.", path);
					return DC_STATUS_TIMEOUT;
				}
				WARNING (abstract->context, "Skipping frame from another client while paging %s (op 0x%02x).",
					path, packet[1]);
				continue;
			}
			if (++malformed_skips >= MAX_MALFORMED_SKIPS) {
				ERROR (abstract->context, "Too many malformed frames for %s page %u (" DC_PRINTF_SIZE " bytes).", path, page, len);
				return DC_STATUS_DATAFORMAT;
			}
			WARNING (abstract->context, "Skipping malformed frame while paging %s (" DC_PRINTF_SIZE " bytes).", path, len);
		}

		unsigned int frame_status = array_uint16_le (packet + RPC_STATUS_OFFSET);
		if (frame_status != RPC_STATUS_OK && frame_status != RPC_STATUS_CONTINUE) {
			ERROR (abstract->context, "Watch returned status %u for %s page %u.", frame_status, path, page);
			return DC_STATUS_PROTOCOL;
		}

		if (len > header) {
			if (!dc_buffer_append (response, packet + header, len - header)) {
				ERROR (abstract->context, "Failed to allocate memory.");
				return DC_STATUS_NOMEMORY;
			}
			offset += (unsigned int) (len - header);
		}

		if (frame_status == RPC_STATUS_OK) {
			DEBUG (abstract->context, "Paginated fetch done: %u page(s), " DC_PRINTF_SIZE " bytes for %s.",
				page + 1, dc_buffer_get_size (response), path);
			return DC_STATUS_SUCCESS;
		}
	}

	WARNING (abstract->context, "Paginated fetch hit the page limit for %s -- data may be truncated.", path);
	return DC_STATUS_SUCCESS;
}

// Fetch a small whole resource (e.g. /Logbook/Entries) via the official
// app's GET -> ACK(handle) -> SHORT-FETCH(0x0D) -> DATA(0x05) flow. The
// listing endpoints don't answer the 0x0B/0x10 stream-fetch triggers, only
// this no-range 0x0D form. `response` receives the raw DATA-frame content
// (everything after the A5 05 sublen header). Assumes the resource fits in
// one DATA frame, which holds for a normal logbook.
// GET -> ACK(handle) -> 0x0D short fetch -> read one frame. Returns the RAW
// frame bytes (the whole A5.. packet) in `frame`, with NO opcode validation.
// suunto_nautic_device_short_fetch() validates and extracts the content.
static dc_status_t
suunto_nautic_short_fetch_frame (dc_device_t *abstract, const char *path, dc_buffer_t *frame, int skip_non_data)
{
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;

	// 1. GET the resource; the ACK carries the 3-byte session handle.
	dc_buffer_t *ack = dc_buffer_new (0);
	if (ack == NULL)
		return DC_STATUS_NOMEMORY;

	status = suunto_nautic_device_request (abstract, path, ack);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "Failed to request %s.", path);
		return status;
	}

	const unsigned char *ack_data = dc_buffer_get_data (ack);
	size_t ack_size = dc_buffer_get_size (ack);
	if (ack_size < RPC_HANDLE_OFFSET + 3) {
		dc_buffer_free (ack);
		ERROR (abstract->context, "ACK too short for a handle (" DC_PRINTF_SIZE ").", ack_size);
		return DC_STATUS_DATAFORMAT;
	}
	unsigned char handle[3];
	memcpy (handle, ack_data + RPC_HANDLE_OFFSET, sizeof (handle));
	dc_buffer_free (ack);

	// 2. SHORT fetch (no range header) reads the whole small resource.
	unsigned char fetch[32];
	unsigned int fetch_len = 0;
	status = suunto_nautic_build_short_fetch (fetch, sizeof (fetch), &fetch_len, device->sequence, handle);
	if (status != DC_STATUS_SUCCESS)
		return status;
	device->sequence++;

	status = dc_iostream_write (device->iostream, fetch, fetch_len, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the short fetch for %s.", path);
		return status;
	}

	// 3. Read the response frame. The watch occasionally injects an
	// unsolicited Hello frame (op 0x12/0x13) mid-session, and a single blind
	// read can grab that instead of the DATA frame -- an intermittent
	// DC_STATUS_DATAFORMAT. When skip_non_data is set, skip frames that aren't
	// a DATA (0x05) frame until the real one arrives (bounded). The raw
	// diagnostic passes 0 and returns the very first frame, whatever it is.
	unsigned char packet[MAX_PACKET] = {0};
	size_t len = 0;
	unsigned int foreign_skips = 0;
	unsigned int malformed_skips = 0;
	for (;;) {
		status = dc_iostream_read (device->iostream, packet, sizeof (packet), &len);
		if (status != DC_STATUS_SUCCESS) {
			// A contended link (another client holding a subscription) means our
			// DATA never gets a turn and this read times out. Surface it as-is
			// (DC_STATUS_TIMEOUT) rather than a misleading DATAFORMAT.
			ERROR (abstract->context, "Failed to receive the data for %s.", path);
			return status;
		}
		HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "FETCH RSP", packet, len);
		if (!skip_non_data)
			break;
		// Accept only a DATA (0x05) frame whose 3-byte handle matches the one
		// this fetch was issued against. The watch interleaves unsolicited frames
		// (a re-sent Hello, an analytics/event stream), and another client (the
		// Suunto app's logbook subscription: 0x07 subscribe-result traffic on a
		// different handle) can flood the shared link. A blind read would grab
		// those -- surfacing as an intermittent DC_STATUS_DATAFORMAT or, worse,
		// the wrong resource's bytes. Skip anything that isn't our DATA frame.
		if (suunto_nautic_frame_is_our_data (packet, len, handle))
			break;
		if (suunto_nautic_frame_wellformed (packet, len)) {
			if (++foreign_skips >= MAX_FOREIGN_SKIPS) {
				ERROR (abstract->context, "Link saturated by another client while fetching %s; giving up.", path);
				return DC_STATUS_TIMEOUT;
			}
			WARNING (abstract->context, "Skipping frame from another client while fetching %s (op 0x%02x).",
				path, packet[1]);
			continue;
		}
		if (++malformed_skips >= MAX_MALFORMED_SKIPS) {
			ERROR (abstract->context, "Too many malformed frames while fetching %s (" DC_PRINTF_SIZE " bytes).", path, len);
			return DC_STATUS_DATAFORMAT;
		}
		WARNING (abstract->context, "Skipping malformed frame while fetching %s (" DC_PRINTF_SIZE " bytes).", path, len);
	}

	dc_buffer_clear (frame);
	if (!dc_buffer_append (frame, packet, len)) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_device_short_fetch (dc_device_t *abstract, const char *path, dc_buffer_t *response)
{
	dc_buffer_t *frame = dc_buffer_new (0);
	if (frame == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t status = suunto_nautic_short_fetch_frame (abstract, path, frame, 1);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (frame);
		return status;
	}

	const unsigned char *packet = dc_buffer_get_data (frame);
	size_t len = dc_buffer_get_size (frame);

	if (len < RPC_STATUS_OFFSET + 2 || packet[0] != 0xA5 || packet[1] != RPC_OP_DATA) {
		ERROR (abstract->context, "Unexpected data frame for %s (" DC_PRINTF_SIZE " bytes).", path, len);
		dc_buffer_free (frame);
		return DC_STATUS_DATAFORMAT;
	}

	// 200 = the whole resource fits in this one frame. 100 = a paginated first
	// page: the watch has more entries than one page holds and would serve the
	// rest via a continuation request. The continuation is not followed yet, so
	// accept the page we got (the most recent dives) rather than failing;
	// following it would return the older dives beyond this page.
	unsigned int frame_status = array_uint16_le (packet + RPC_STATUS_OFFSET);
	if (frame_status != RPC_STATUS_OK && frame_status != RPC_STATUS_CONTINUE) {
		ERROR (abstract->context, "Watch returned status %u for %s (200/100 expected).", frame_status, path);
		dc_buffer_free (frame);
		return DC_STATUS_PROTOCOL;
	}

	// Return the frame content (everything after A5 05 sublen).
	dc_buffer_clear (response);
	int ok = dc_buffer_append (response, packet + 4, len - 4);
	dc_buffer_free (frame);
	if (!ok) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_device_download_summary (dc_device_t *abstract, const char *logbook_id, dc_buffer_t *summary)
{
	if (abstract == NULL || abstract->vtable->type != DC_FAMILY_SUUNTO_NAUTIC || logbook_id == NULL || summary == NULL)
		return DC_STATUS_INVALIDARGS;

	char path[128];
	int n = snprintf (path, sizeof (path), "/Logbook/byId/%s/Summary", logbook_id);
	if (n < 0 || (size_t) n >= sizeof (path))
		return DC_STATUS_INVALIDARGS;

	// /Summary uses the paginated 0x0D fetch and is NOT compressed -- the
	// result is raw SBEM0103 (the caller locates the signature and reads
	// its fields, e.g. gradient factors and gas mix).
	return suunto_nautic_device_paginated_fetch (abstract, path, summary);
}

static dc_status_t
suunto_nautic_device_download (dc_device_t *abstract, const char *logbook_id, dc_buffer_t *raw)
{
	if (abstract == NULL || abstract->vtable->type != DC_FAMILY_SUUNTO_NAUTIC || logbook_id == NULL || raw == NULL)
		return DC_STATUS_INVALIDARGS;

	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;

	char path[128];
	int n = snprintf (path, sizeof (path), "/Logbook/byId/%s/Data", logbook_id);
	if (n < 0 || (size_t) n >= sizeof (path))
		return DC_STATUS_INVALIDARGS;

	dc_buffer_t *compressed = dc_buffer_new (0);
	if (compressed == NULL)
		return DC_STATUS_NOMEMORY;

	// The stream_fetch above now closes each stream with a STREAM_STOP, so the
	// watch releases its handle between dives and back-to-back downloads no
	// longer collide. This retry stays as a safety net: if a stream is still
	// refused (status 423 Locked) or comes back empty, back off -- giving the
	// watch time to finish tearing down -- and try again before skipping the
	// dive.
	for (unsigned int attempt = 0; attempt < SUUNTO_NAUTIC_DOWNLOAD_RETRIES; attempt++) {
		if (attempt > 0) {
			WARNING (abstract->context, "Retrying the download of %s (attempt %u/%u).",
				logbook_id, attempt + 1, SUUNTO_NAUTIC_DOWNLOAD_RETRIES);
			dc_iostream_sleep (device->iostream, 1500 * attempt);
		}
		dc_buffer_clear (compressed);
		status = suunto_nautic_device_stream_fetch (abstract, path, compressed);
		if (status == DC_STATUS_SUCCESS && dc_buffer_get_size (compressed) > 0)
			break;
		status = DC_STATUS_PROTOCOL;
	}
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (compressed);
		return status;
	}

	DEBUG (abstract->context, "Captured " DC_PRINTF_SIZE " compressed bytes for logbook entry %s.",
		dc_buffer_get_size (compressed), logbook_id);

	// Decompress and verify the SBEM0103 magic.
	status = suunto_nautic_heatshrink_decompress (abstract->context,
		dc_buffer_get_data (compressed), dc_buffer_get_size (compressed), raw);
	dc_buffer_free (compressed);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to decompress the logbook entry.");
		return status;
	}

	if (dc_buffer_get_size (raw) < sizeof (SBEM_MAGIC) ||
		memcmp (dc_buffer_get_data (raw), SBEM_MAGIC, sizeof (SBEM_MAGIC)) != 0) {
		ERROR (abstract->context, "Unexpected magic in the decompressed data.");
		return DC_STATUS_DATAFORMAT;
	}

	DEBUG (abstract->context, "Decompressed " DC_PRINTF_SIZE " bytes for logbook entry %s.",
		dc_buffer_get_size (raw), logbook_id);

	// Append the /Summary SBEM (gradient factors, gas mix) after the
	// profile, so the parser can expose them via DC_FIELD_DECOMODEL /
	// DC_FIELD_GASMIX -- these aren't in the profile stream. Best-effort:
	// the profile alone is still a valid dive if this fails.
	dc_buffer_t *summary = dc_buffer_new (0);
	if (summary != NULL) {
		if (suunto_nautic_device_download_summary (abstract, logbook_id, summary) == DC_STATUS_SUCCESS &&
			dc_buffer_get_size (summary) > 0) {
			if (!dc_buffer_append (raw, dc_buffer_get_data (summary), dc_buffer_get_size (summary)))
				WARNING (abstract->context, "Failed to append the Summary; GF/gas will be unavailable.");
		} else {
			WARNING (abstract->context, "Failed to fetch the Summary for %s; GF/gas will be unavailable.", logbook_id);
		}
		dc_buffer_free (summary);
	}

	return DC_STATUS_SUCCESS;
}

// Extract dive-start ids from a /Logbook/Entries response, newest-first. Each
// entry is a start timestamp immediately followed by its end timestamp
// (end > start, within a day), both 4-aligned little-endian uint32s in the
// dive-ID window; a lone in-range value with no paired end is a header/misc
// field (the response's own "current time"), not a dive. Writes up to max_ids
// ids and returns the count.
static unsigned int
suunto_nautic_extract_entry_ids (const unsigned char *data, size_t size,
	unsigned int *ids, unsigned int max_ids)
{
	unsigned int count = 0;
	for (size_t i = 0; i + 8 <= size && count < max_ids; i += 4) {
		unsigned int v = array_uint32_le (data + i);
		if (v < DIVE_ID_MIN || v > DIVE_ID_MAX)
			continue;
		unsigned int next = array_uint32_le (data + i + 4);
		if (next >= DIVE_ID_MIN && next <= DIVE_ID_MAX &&
				next > v && next - v <= DIVE_ENTRY_MAX_PAIR_GAP) {
			ids[count++] = v; // start of a (start, end) pair
			i += 4;           // skip the paired end timestamp
		}
	}
	// Sort descending (newest first); insertion sort is fine at logbook scale.
	for (unsigned int a = 1; a < count; a++) {
		unsigned int key = ids[a];
		int b = (int) a - 1;
		while (b >= 0 && ids[b] < key) { ids[b + 1] = ids[b]; b--; }
		ids[b + 1] = key;
	}
	return count;
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

/*
 * Best-effort device info. A short fetch of /Info returns a device-identity
 * record: a run of NUL-separated strings mixed in with binary framing bytes,
 *
 *   Suunto\0 Nautic\0 Vaasa\0 T\0 2604C3003306\0 01385D42<hwpart><...>\0
 *     2.55.46\0 <hwpart>\0 ... SIM\0 <mac>\0 BID\0 <build>\0
 *     BLE Mac Address\0 <mac>\0 WiFi Mac Address\0 <mac>\0
 *
 * (confirmed on a live Nautic, firmware 2.55.46, via fetch_device_info.py).
 * The watch serial is the first 12-char uppercase-hex token and the firmware
 * the first "N.N.N" token; the serial always precedes the firmware, and the
 * BLE/WiFi MACs (also 12 hex) come after it -- so stop looking for a serial
 * once the firmware token is seen. Layered on top of the download; every
 * failure path here is non-fatal.
 *
 * dc_event_devinfo_t carries unsigned ints only: the firmware is packed
 * (a << 16) | (b << 8) | c and the hex serial truncated to its low 32 bits.
 * A consumer wanting the faithful strings should read the BLE advertised
 * name (serial) or GET /Info directly (firmware).
 */
static void
suunto_nautic_emit_devinfo (dc_device_t *abstract)
{
	dc_buffer_t *info = dc_buffer_new (0);
	if (info == NULL)
		return;

	if (suunto_nautic_device_short_fetch (abstract, "/Info", info) != DC_STATUS_SUCCESS) {
		dc_buffer_free (info);
		return; // not fatal -- just no device info this time
	}

	const unsigned char *d = dc_buffer_get_data (info);
	size_t n = dc_buffer_get_size (info);

	dc_event_devinfo_t devinfo;
	memset (&devinfo, 0, sizeof (devinfo));

	// Walk NUL-delimited tokens; the record's strings are NUL-terminated, and
	// the binary framing bytes between them fail both tests.
	size_t start = 0;
	for (size_t i = 0; i < n; i++) {
		if (d[i] != 0)
			continue;
		const char *tok = (const char *) (d + start);
		size_t toklen = i - start;
		unsigned int a, b, c;
		if (!devinfo.firmware && suunto_nautic_parse_version (tok, toklen, &a, &b, &c))
			devinfo.firmware = (a << 16) | (b << 8) | c;
		else if (!devinfo.firmware && !devinfo.serial && suunto_nautic_is_serial (tok, toklen)) {
			char buf[13];
			memcpy (buf, tok, 12);
			buf[12] = 0;
			devinfo.serial = (unsigned int) strtoul (buf, NULL, 16);
		}
		start = i + 1;
	}

	dc_buffer_free (info);

	if (devinfo.firmware || devinfo.serial) {
		INFO (abstract->context, "Device info: firmware=%u.%u.%u serial=0x%08x",
			(devinfo.firmware >> 16) & 0xFF, (devinfo.firmware >> 8) & 0xFF,
			devinfo.firmware & 0xFF, devinfo.serial);
		device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);
	}
}

static dc_status_t
suunto_nautic_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_device_t *device = (suunto_nautic_device_t *) abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Connectivity/auth check. Any path works here; /System/Mode is a
	// fixed, id-less endpoint so it works identically on every unit.
	dc_buffer_t *mode = dc_buffer_new (0);
	if (mode == NULL)
		return DC_STATUS_NOMEMORY;

	status = suunto_nautic_device_request (abstract, "/System/Mode", mode);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (mode);
		ERROR (abstract->context, "Failed to reach /System/Mode. The EVA handshake or RPC "
			"framing may need updating for this device (see suunto_nautic.h).");
		return status;
	}

	dc_event_vendor_t vendor;
	vendor.data = dc_buffer_get_data (mode);
	vendor.size = (unsigned int) dc_buffer_get_size (mode);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);
	dc_buffer_free (mode);

	// Best-effort firmware / serial via DC_EVENT_DEVINFO; never fatal.
	suunto_nautic_emit_devinfo (abstract);

	progress.current = 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// /Logbook/Entries returns a small SBEM payload embedding each dive's
	// LogId (a UNIX timestamp) as a 4-aligned little-endian uint32 among
	// handle/flag/count/CRC fields; a timestamp-window filter (DIVE_ID_MIN/
	// MAX) isolates the IDs. Uses the short 0x0D fetch -- the watch rejects
	// the ranged stream-fetch (used for dive data) here.
	dc_buffer_t *entries = dc_buffer_new (0);
	if (entries == NULL)
		return DC_STATUS_NOMEMORY;

	status = suunto_nautic_device_short_fetch (abstract, "/Logbook/Entries", entries);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free (entries);
		ERROR (abstract->context, "Failed to fetch /Logbook/Entries.");
		return status;
	}

	const unsigned char *entries_data = dc_buffer_get_data (entries);
	size_t entries_size = dc_buffer_get_size (entries);

	size_t max_ids = entries_size / 4 + 1; // at most one id per 4 bytes
	unsigned int *ids = (unsigned int *) malloc (max_ids * sizeof (unsigned int));
	if (ids == NULL) {
		dc_buffer_free (entries);
		return DC_STATUS_NOMEMORY;
	}
	unsigned int count = suunto_nautic_extract_entry_ids (entries_data, entries_size,
		ids, (unsigned int) max_ids);
	dc_buffer_free (entries);

	progress.maximum = (count + 1) * 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *raw = dc_buffer_new (0);
	if (raw == NULL) {
		free (ids);
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < count; i++) {
		unsigned char fingerprint[4] = {
			(unsigned char) (ids[i] & 0xFF),
			(unsigned char) ((ids[i] >> 8) & 0xFF),
			(unsigned char) ((ids[i] >> 16) & 0xFF),
			(unsigned char) ((ids[i] >> 24) & 0xFF),
		};

		// Walking newest-first, so the first fingerprint match means
		// everything from here on was already downloaded in a
		// previous session.
		if (memcmp (fingerprint, device->fingerprint, sizeof (fingerprint)) == 0)
			break;

		char logbook_id[16];
		int n = snprintf (logbook_id, sizeof (logbook_id), "%u", ids[i]);
		if (n < 0 || (size_t) n >= sizeof (logbook_id))
			continue;

		dc_buffer_clear (raw);
		status = suunto_nautic_device_download (abstract, logbook_id, raw);
		if (status != DC_STATUS_SUCCESS) {
			// A logbook can contain empty/aborted entries (a zero-length
			// session is listed in /Logbook/Entries but downloads to no
			// profile data and fails the SBEM magic check). Skip with a
			// warning rather than aborting the whole enumeration.
			WARNING (abstract->context, "Skipping logbook entry %s (download failed, likely an empty/aborted dive).", logbook_id);
			status = DC_STATUS_SUCCESS;
			continue;
		}

		progress.current += 2;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		if (callback && !callback (dc_buffer_get_data (raw), (unsigned int) dc_buffer_get_size (raw),
			fingerprint, sizeof (fingerprint), userdata))
			break;
	}

	dc_buffer_free (raw);
	free (ids);

	return DC_STATUS_SUCCESS;
}
