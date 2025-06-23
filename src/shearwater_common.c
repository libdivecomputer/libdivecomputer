/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include "shearwater_common.h"

#include "context-private.h"
#include "platform.h"
#include "array.h"

#define SZ_PACKET  254

// SLIP special character codes
#define END       0xC0
#define ESC       0xDB
#define ESC_END   0xDC
#define ESC_ESC   0xDD

#define RDBI_REQUEST  0x22
#define RDBI_RESPONSE 0x62

#define WDBI_REQUEST  0x2E
#define WDBI_RESPONSE 0x6E

#define NAK 0x7F

dc_status_t
shearwater_common_setup (shearwater_common_device_t *device, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	device->iostream = iostream;

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		return status;
	}

	// Set the timeout for receiving data (3000ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		return status;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	return DC_STATUS_SUCCESS;
}


static int
shearwater_common_decompress_lre (unsigned char *data, unsigned int size, dc_buffer_t *buffer, unsigned int *isfinal)
{
	// The RLE decompression algorithm does interpret the binary data as a
	// stream of 9 bit values. Therefore, the total number of bits needs to be
	// a multiple of 9 bits.
	unsigned int nbits = size * 8;
	if (nbits % 9 != 0)
		return -1;

	unsigned int offset = 0;
	while (offset + 9 <= nbits) {
		// Extract the 9 bit value.
		unsigned int byte = offset / 8;
		unsigned int bit  = offset % 8;
		unsigned int shift = 16 - (bit + 9);
		unsigned int value = (array_uint16_be (data + byte) >> shift) & 0x1FF;

		// The 9th bit indicates whether the remaining 8 bits represent
		// a run of zero bytes or not. If the bit is set, the value is
		// not a run and doesn't need expansion. If the bit is not set,
		// the value contains the number of zero bytes in the run. A
		// zero-length run indicates the end of the compressed stream.
		if (value & 0x100) {
			// Append the data byte directly.
			unsigned char c = value & 0xFF;
			if (!dc_buffer_append (buffer, &c, 1))
				return -1;
		} else if (value == 0) {
			// Reached the end of the compressed stream.
			if (isfinal)
				*isfinal = 1;
			break;
		} else {
			// Expand the run with zero bytes.
			if (!dc_buffer_resize (buffer, dc_buffer_get_size (buffer) + value))
				return -1;
		}

		offset += 9;
	}

	return 0;
}


static int
shearwater_common_decompress_xor (unsigned char *data, unsigned int size)
{
	// Each block of 32 bytes is XOR'ed (in-place) with the previous block,
	// except for the first block, which is passed through unchanged.
	for (unsigned int i = 32; i < size; ++i) {
		data[i] ^= data[i - 32];
	}

	return 0;
}

static dc_status_t
shearwater_common_slip_write (shearwater_common_device_t *device, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);
	unsigned char buffer[32];
	unsigned int nbytes = 0;

	if (transport == DC_TRANSPORT_BLE) {
		// Calculate the total number of bytes.
		unsigned int count = 1;
		for (unsigned int i = 0; i < size; ++i) {
			unsigned char c = data[i];
			if (c == END || c == ESC) {
				count += 2;
			} else {
				count++;
			}
		}

		// Calculate the total number of frames.
		unsigned int nframes = (count + sizeof(buffer) - 1) / sizeof(buffer);

		buffer[0] = nframes;
		buffer[1] = 0;
		nbytes = 2;
	}

	for (unsigned int i = 0; i < size; ++i) {
		unsigned char c = data[i];

		if (c == END || c == ESC) {
			// Append the escape character.
			buffer[nbytes++] = ESC;

			// Flush the buffer if necessary.
			if (nbytes >= sizeof(buffer)) {
				status = dc_iostream_write (device->iostream, buffer, nbytes, NULL);
				if (status != DC_STATUS_SUCCESS) {
					ERROR (device->base.context, "Failed to send the packet.");
					return status;
				}

				if (transport == DC_TRANSPORT_BLE) {
					buffer[1]++;
					nbytes = 2;
				} else {
					nbytes = 0;
				}
			}

			// Escape the character.
			if (c == END) {
				c = ESC_END;
			} else {
				c = ESC_ESC;
			}
		}

		// Append the character.
		buffer[nbytes++] = c;

		// Flush the buffer if necessary.
		if (nbytes >= sizeof(buffer)) {
			status = dc_iostream_write (device->iostream, buffer, nbytes, NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (device->base.context, "Failed to send the packet.");
				return status;
			}

			if (transport == DC_TRANSPORT_BLE) {
				buffer[1]++;
				nbytes = 2;
			} else {
				nbytes = 0;
			}
		}
	}

	// Append the END character to indicate the end of the packet.
	buffer[nbytes++] = END;

	// Flush the buffer.
	status = dc_iostream_write (device->iostream, buffer, nbytes, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the packet.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_common_slip_read (shearwater_common_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);
	unsigned char buffer[256];
	unsigned int escaped = 0;
	unsigned int nbytes = 0;

	// Get the packet size.
	size_t packetsize = (transport == DC_TRANSPORT_BLE) ? sizeof(buffer) : 1;

	// Read bytes until a complete packet has been received. If the
	// buffer runs out of space, bytes are dropped. The caller can
	// detect this condition because the return value will be larger
	// than the supplied buffer size.
	while (1) {
		size_t transferred = 0;
		status = dc_iostream_read (device->iostream, buffer, packetsize, &transferred);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (device->base.context, "Failed to receive the packet.");
			return status;
		}

		size_t offset = 0;
		if (transport == DC_TRANSPORT_BLE) {
			if (transferred < 2) {
				ERROR (device->base.context, "Invalid packet length (" DC_PRINTF_SIZE ").", transferred);
				return DC_STATUS_PROTOCOL;
			}

			offset = 2;
		}

		for (size_t i = offset; i < transferred; ++i) {
			unsigned char c = buffer[i];

			if (c == END || c == ESC) {
				if (escaped) {
					// If the END or ESC characters are escaped, then we
					// have a protocol violation, and an error is reported.
					ERROR (device->base.context, "SLIP frame escaped the special character %02x.", c);
					return DC_STATUS_PROTOCOL;
				}

				if (c == END) {
					// If it's an END character then we're done.
					// As a minor optimization, empty packets are ignored. This
					// is to avoid bothering the upper layers with all the empty
					// packets generated by the duplicate END characters which
					// are sent to try to detect line noise.
					if (nbytes) {
						goto done;
					}
				} else {
					// If it's an ESC character, get another character and then
					// figure out what to store in the packet based on that.
					escaped = 1;
				}

				continue;
			}

			if (escaped) {
				// If it's not one of the two escaped characters, then we
				// have a protocol violation. The best bet seems to be to
				// leave the byte alone and just stuff it into the packet.
				switch (c) {
				case ESC_END:
					c = END;
					break;
				case ESC_ESC:
					c = ESC;
					break;
				default:
					break;
				}

				escaped = 0;
			}

			if (nbytes < size)
				data[nbytes] = c;
			nbytes++;
		}
	}

done:

	if (nbytes > size) {
		ERROR (device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_PROTOCOL;
	}

	if (actual)
		*actual = nbytes;

	return status;
}


dc_status_t
shearwater_common_transfer (shearwater_common_device_t *device, const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[SZ_PACKET + 4];
	unsigned int n = 0;

	if (isize > SZ_PACKET || osize > SZ_PACKET)
		return DC_STATUS_INVALIDARGS;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Setup the request packet.
	packet[0] = 0xFF;
	packet[1] = 0x01;
	packet[2] = isize + 1;
	packet[3] = 0x00;
	memcpy (packet + 4, input, isize);

	// Send the request packet.
	status = shearwater_common_slip_write (device, packet, isize + 4);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the request packet.");
		return status;
	}

	// Return early if no response packet is requested.
	if (osize == 0) {
		if (actual)
			*actual = 0;
		return DC_STATUS_SUCCESS;
	}

	// Receive the response packet.
	status = shearwater_common_slip_read (device, packet, sizeof (packet), &n);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the response packet.");
		return status;
	}

	// Validate the packet header.
	if (n < 4 || packet[0] != 0x01 || packet[1] != 0xFF || packet[3] != 0x00) {
		ERROR (abstract->context, "Invalid packet header.");
		return DC_STATUS_PROTOCOL;
	}

	// Validate the packet length.
	unsigned int length = packet[2];
	if (length < 1 || length - 1 + 4 != n || length - 1 > osize) {
		ERROR (abstract->context, "Invalid packet header.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy (output, packet + 4, length - 1);
	if (actual)
		*actual = length - 1;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_common_download (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int address, unsigned int size, unsigned int compression, dc_event_progress_t *progress)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned int n = 0;

	unsigned char req_init[] = {
		0x35,
		(compression ? 0x10 : 0x00),
		0x34,
		(address >> 24) & 0xFF,
		(address >> 16) & 0xFF,
		(address >>  8) & 0xFF,
		(address      ) & 0xFF,
		(size >> 16) & 0xFF,
		(size >>  8) & 0xFF,
		(size      ) & 0xFF};
	unsigned char req_block[] = {0x36, 0x00};
	unsigned char req_quit[] = {0x37};
	unsigned char response[SZ_PACKET];

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	unsigned int initial = 0, current = 0, maximum = 3 + size + 1;
	if (progress) {
		initial = progress->current;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	// Transfer the init request.
	rc = shearwater_common_transfer (device, req_init, sizeof (req_init), response, 3, &n);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	// Verify the init response.
	if (n != 3 || response[0] != 0x75 || response[1] != 0x10 || response[2] > SZ_PACKET) {
		ERROR (abstract->context, "Unexpected response packet.");
		return DC_STATUS_PROTOCOL;
	}

	// Update and emit a progress event.
	if (progress) {
		current += 3;
		progress->current = initial + STEP (current, maximum);
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	unsigned int done = 0;
	unsigned char block = 1;
	unsigned int nbytes = 0;
	while (nbytes < size && !done) {
		// Transfer the block request.
		req_block[1] = block;
		rc = shearwater_common_transfer (device, req_block, sizeof (req_block), response, sizeof (response), &n);
		if (rc != DC_STATUS_SUCCESS) {
			return rc;
		}

		// Verify the block header.
		if (n < 2 || response[0] != 0x76 || response[1] != block) {
			ERROR (abstract->context, "Unexpected response packet.");
			return DC_STATUS_PROTOCOL;
		}

		// Verify the block length.
		unsigned int length = n - 2;
		if (nbytes + length > size) {
			ERROR (abstract->context, "Unexpected packet size.");
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		if (progress) {
			current += length;
			progress->current = initial + STEP (current, maximum);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		if (compression) {
			if (shearwater_common_decompress_lre (response + 2, length, buffer, &done) != 0) {
				ERROR (abstract->context, "Decompression error (LRE phase).");
				return DC_STATUS_PROTOCOL;
			}
		} else {
			if (!dc_buffer_append (buffer, response + 2, length)) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				return DC_STATUS_PROTOCOL;
			}
		}

		nbytes += length;
		block++;
	}

	if (compression) {
		if (shearwater_common_decompress_xor (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer)) != 0) {
			ERROR (abstract->context, "Decompression error (XOR phase).");
			return DC_STATUS_PROTOCOL;
		}
	}

	// Transfer the quit request.
	rc = shearwater_common_transfer (device, req_quit, sizeof (req_quit), response, 2, &n);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	// Verify the quit response.
	if (n != 2 || response[0] != 0x77 || response[1] != 0x00) {
		ERROR (abstract->context, "Unexpected response packet.");
		return DC_STATUS_PROTOCOL;
	}

	// Update and emit a progress event.
	if (progress) {
		current += 1;
		progress->current = initial + STEP (current, maximum);
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_common_rdbi (shearwater_common_device_t *device, unsigned int id, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Transfer the request.
	unsigned int n = 0;
	unsigned char request[] = {
		RDBI_REQUEST,
		(id >> 8) & 0xFF,
		(id     ) & 0xFF};
	unsigned char response[SZ_PACKET];
	status = shearwater_common_transfer (device, request, sizeof (request), response, sizeof (response), &n);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Verify the response.
	if (n < 3 || response[0] != RDBI_RESPONSE || response[1] != request[1] || response[2] != request[2]) {
		if (n == 3 && response[0] == NAK && response[1] == RDBI_REQUEST) {
			ERROR (abstract->context, "Received NAK packet with error code 0x%02x.", response[2]);
			return DC_STATUS_UNSUPPORTED;
		}
		ERROR (abstract->context, "Unexpected response packet.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int length = n - 3;

	if (length > size) {
		ERROR (abstract->context, "Unexpected packet size (%u bytes).", length);
		return DC_STATUS_PROTOCOL;
	}

	if (actual == NULL) {
		// Verify the actual length.
		if (length != size) {
			ERROR (abstract->context, "Unexpected packet size (%u bytes).", length);
			return DC_STATUS_PROTOCOL;
		}
	} else {
		// Return the actual length.
		*actual = length;
	}

	if (length) {
		memcpy (data, response + 3, length);
	}

	return status;
}

dc_status_t
shearwater_common_wdbi (shearwater_common_device_t *device, unsigned int id, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size + 3 > SZ_PACKET) {
		return DC_STATUS_INVALIDARGS;
	}

	// Transfer the request.
	unsigned int n = 0;
	unsigned char request[SZ_PACKET] = {
		WDBI_REQUEST,
		(id >> 8) & 0xFF,
		(id     ) & 0xFF};
	if (size) {
		memcpy (request + 3, data, size);
	}
	unsigned char response[SZ_PACKET];
	status = shearwater_common_transfer (device, request, size + 3, response, sizeof (response), &n);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Verify the response.
	if (n < 3 || response[0] != WDBI_RESPONSE || response[1] != request[1] || response[2] != request[2]) {
		if (n == 3 && response[0] == NAK && response[1] == WDBI_REQUEST) {
			ERROR (abstract->context, "Received NAK packet with error code 0x%02x.", response[2]);
			return DC_STATUS_UNSUPPORTED;
		}
		ERROR (abstract->context, "Unexpected response packet.");
		return DC_STATUS_PROTOCOL;
	}

	return status;
}

dc_status_t
shearwater_common_timesync_local (shearwater_common_device_t *device, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Convert to local time.
	dc_datetime_t local = *datetime;
	local.timezone = DC_TIMEZONE_NONE;

	dc_ticks_t ticks = dc_datetime_mktime (&local);
	if (ticks == -1) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	const unsigned char timestamp[] = {
		(ticks >> 24) & 0xFF,
		(ticks >> 16) & 0xFF,
		(ticks >>  8) & 0xFF,
		(ticks      ) & 0xFF,
	};

	status = shearwater_common_wdbi (device, ID_TIME_LOCAL, timestamp, sizeof(timestamp));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to write the dive computer local time.");
		return status;
	}

	return status;
}

dc_status_t
shearwater_common_timesync_utc (shearwater_common_device_t *device, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Convert to UTC time.
	dc_ticks_t ticks = dc_datetime_mktime (datetime);
	if (ticks == -1) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	const unsigned char timestamp[] = {
		(ticks >> 24) & 0xFF,
		(ticks >> 16) & 0xFF,
		(ticks >>  8) & 0xFF,
		(ticks      ) & 0xFF,
	};

	status = shearwater_common_wdbi (device, ID_TIME_UTC, timestamp, sizeof(timestamp));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to write the dive computer UTC time.");
		return status;
	}

	int timezone = datetime->timezone / 60;
	const unsigned char offset[] = {
		(timezone >> 24) & 0xFF,
		(timezone >> 16) & 0xFF,
		(timezone >>  8) & 0xFF,
		(timezone      ) & 0xFF,
	};

	status = shearwater_common_wdbi (device, ID_TIME_OFFSET, offset, sizeof (offset));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to write the dive computer timezone offset.");
		return status;
	}

	// We don't have a way to determine the daylight savings time setting,
	// but the required offset is already factored into the timezone offset.
	const unsigned char dst[] = {0, 0, 0, 0};

	status = shearwater_common_wdbi (device, ID_TIME_DST, dst, sizeof (dst));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to write the dive computer DST setting.");
		return status;
	}

	return status;
}

unsigned int
shearwater_common_get_model (shearwater_common_device_t *device, unsigned int hardware)
{
	unsigned int model = 0;

	switch (hardware) {
	case 0x0101:
	case 0x0202:
		model = PREDATOR;
		break;
	case 0x0404:
	case 0x0909:
		model = PETREL;
		break;
	case 0x0505:
	case 0x0808:
	case 0x0838:
	case 0x08A5:
	case 0x0B0B:
	case 0x7828:
	case 0x7B2C:
	case 0x8838:
		model = PETREL2;
		break;
	case 0xB407:
		model = PETREL3;
		break;
	case 0x0606:
	case 0x0A0A:
		model = NERD;
		break;
	case 0x0E0D:
	case 0x7E2D:
		model = NERD2;
		break;
	case 0x0707:
		model = PERDIX;
		break;
	case 0x0C0D:
	case 0x7C2D:
	case 0x8D6C:
		model = PERDIXAI;
		break;
	case 0x704C:
	case 0xC407:
	case 0xC964:
	case 0x9C64:
		model = PERDIX2;
		break;
	case 0x0F0F:
	case 0x1F0A:
	case 0x1F0F:
		model = TERIC;
		break;
	case 0x1512:
		model = PEREGRINE;
		break;
	case 0x1712:
		model = PEREGRINE_TX;
		break;
	case 0xC0E0:
		model = TERN;
		break;
	default:
		WARNING (device->base.context, "Unknown hardware type 0x%04x.", hardware);
	}

	return model;
}
