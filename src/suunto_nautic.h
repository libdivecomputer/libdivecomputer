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

/*
 * Suunto Nautic / Ocean ("Vaasa" generation) BLE dive computers.
 *
 * Transport and framing:
 *   - BLE, with HDLC framing (flag 0x7E, escape 0x7D / XOR 0x20; no
 *     separate checksum at this layer).
 *   - Path-addressed GET requests use the RPC frame envelope:
 *       0xA5, opcode, sublen(u16 LE), seq(u16 LE), 0x01, 0x80, 0x00,
 *       pathlen(u8), path bytes..., crc32(u32 LE)
 *     where crc32 is the reflected CRC-32 (checksum_crc32r) of every
 *     preceding byte. A single monotonic per-connection sequence counter
 *     is used for all requests.
 *
 * Dive download:
 *   - GET <entry>/Data (opcode 0x0A) -> ack -> two stream-fetch triggers
 *     -> chunk stream (opcode 0x01, repeated). The chunk stream is
 *     unacknowledged and continuous; the host buffers until a 2.0s
 *     silence timeout.
 *   - Each 0x01 frame carries a 28-byte MDS header: payload size is a
 *     u16 LE at offset 20, payload starts at offset 28.
 *   - Payloads are Heatshrink-compressed (LZSS variant, see
 *     src/heatshrink/; window_sz2 = 7, lookahead_sz2 = 5). The decoded
 *     stream is an SBEM0103 container.
 *
 * SBEM0103 container:
 *   - A numeric Type-Length-Value stream: [id: 1 byte][length: 1 byte]
 *     [value: length bytes]; length == 255 means an extended 4-byte LE
 *     length follows before the value. Unknown chunk IDs are skipped.
 *   - Decoded chunks (suunto_nautic_parser.c): 0x12 (1Hz absolute
 *     pressure / temperature), 0x16 (depth, cylinder pressures, NDL,
 *     time-to-surface), 0x0B (GPS), 0x17 (surface pressure ->
 *     DC_FIELD_ATMOSPHERIC), plus dive events and high-rate IMU.
 *   - Series libdivecomputer has no sample type for (battery, GPS
 *     accuracy, 9-axis IMU, dive-route features) are emitted through
 *     DC_SAMPLE_VENDOR tagged SAMPLE_VENDOR_SUUNTO_NAUTIC, each record
 *     led by a VENDOR_KIND_* byte.
 *
 * Enumeration:
 *   - suunto_nautic_device_foreach() fetches /Logbook/Entries (via the
 *     short 0x0D fetch; the listing endpoint rejects the stream fetch
 *     used for dive data) and extracts each dive's LogId, which is a
 *     UNIX timestamp, from the SBEM payload. Dives are downloaded
 *     newest-first, stopping at the first id matching the fingerprint.
 *
 * Datetime:
 *   - The stream carries no wall-clock timestamp except in GPS fixes
 *     (chunk 0x0B), which hold an absolute UTC in milliseconds, so
 *     dc_parser_get_datetime() derives start = gps_utc - gps_rel_time.
 *     A dive with no surface GPS fix has no absolute clock in the
 *     stream; datetime is unsupported for it. The LogId (the dive id) is
 *     itself that timestamp and can serve as a fallback.
 *
 * Resynchronisation:
 *   - Heatshrink decompression can leave localized artifacts (runs of a
 *     repeated byte) that a naive TLV walk would misread as a chunk
 *     header and desync on. suunto_nautic_sbem_next() validates every
 *     fixed-length chunk id (0x08=6, 0x0B=20, 0x0E=6, 0x14=7, 0x16=195,
 *     0x17=14 bytes) against its expected length and, on a mismatch,
 *     rescans forward one byte at a time instead of trusting the header.
 */

#ifndef SUUNTO_NAUTIC_H
#define SUUNTO_NAUTIC_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/iostream.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>
#include <libdivecomputer/buffer.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
suunto_nautic_device_open (dc_device_t **device, dc_context_t *context, dc_iostream_t *iostream);

dc_status_t
suunto_nautic_parser_create (dc_parser_t **parser, dc_context_t *context, const unsigned char data[], size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_NAUTIC_H */
