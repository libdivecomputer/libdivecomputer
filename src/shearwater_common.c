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
#include "array.h"

#define SZ_PACKET  254

// SLIP special character codes
#define END       0xC0
#define ESC       0xDB
#define ESC_END   0xDC
#define ESC_ESC   0xDD

#define EXITCODE(n) ((n) < 0 ? (n) : 0)

dc_status_t
shearwater_common_open (shearwater_common_device_t *device, dc_context_t *context, const char *name)
{
	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_sleep (device->port, 300);
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_common_close (shearwater_common_device_t *device)
{
	// Close the device.
	if (serial_close (device->port) == -1) {
		return DC_STATUS_IO;
	}

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
		// not a run and doesn’t need expansion. If the bit is not set,
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


static int
shearwater_common_slip_write (shearwater_common_device_t *device, const unsigned char data[], unsigned int size)
{
	int n = 0;
	const unsigned char end[] = {END};
	const unsigned char esc_end[] = {ESC, ESC_END};
	const unsigned char esc_esc[] = {ESC, ESC_ESC};
	unsigned char buffer[32];
	unsigned int nbytes = 0;

#if 0
	// Send an initial END character to flush out any data that may have
	// accumulated in the receiver due to line noise.
	n = serial_write (device->port, end, sizeof (end));
	if (n != sizeof (end)) {
		return EXITCODE(n);
	}
#endif

	for (unsigned int i = 0; i < size; ++i) {
		const unsigned char *seq = NULL;
		unsigned int len = 0;
		switch (data[i]) {
		case END:
			// Escape the END character.
			seq = esc_end;
			len = sizeof (esc_end);
			break;
		case ESC:
			// Escape the ESC character.
			seq = esc_esc;
			len = sizeof (esc_esc);
			break;
		default:
			// Normal character.
			seq = data + i;
			len = 1;
			break;
		}

		// Flush the buffer if necessary.
		if (nbytes + len + sizeof(end) > sizeof(buffer)) {
			n = serial_write (device->port, buffer, nbytes);
			if (n != nbytes) {
				return EXITCODE(n);
			}

			nbytes = 0;
		}

		// Append the escaped character.
		memcpy(buffer + nbytes, seq, len);
		nbytes += len;
	}

	// Append the END character to indicate the end of the packet.
	memcpy(buffer + nbytes, end, sizeof(end));
	nbytes += sizeof(end);

	// Flush the buffer.
	n = serial_write (device->port, buffer, nbytes);
	if (n != nbytes) {
		return EXITCODE(n);
	}

	return size;
}


static int
shearwater_common_slip_read (shearwater_common_device_t *device, unsigned char data[], unsigned int size)
{
	unsigned int received = 0;

	// Read bytes until a complete packet has been received. If the
	// buffer runs out of space, bytes are dropped. The caller can
	// detect this condition because the return value will be larger
	// than the supplied buffer size.
	while (1) {
		unsigned char c = 0;
		int n = 0;

		// Get a single character to process.
		n = serial_read (device->port, &c, 1);
		if (n != 1) {
			return EXITCODE(n);
		}

		switch (c) {
		case END:
			// If it's an END character then we're done.
			// As a minor optimization, empty packets are ignored. This
			// is to avoid bothering the upper layers with all the empty
			// packets generated by the duplicate END characters which
			// are sent to try to detect line noise.
			if (received)
				return received;
			else
				break;
		case ESC:
			// If it's an ESC character, get another character and then
			// figure out what to store in the packet based on that.
			n = serial_read (device->port, &c, 1);
			if (n != 1) {
				return EXITCODE(n);
			}

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
			}
			// Fall-through!
		default:
			if (received < size)
				data[received] = c;
			received++;
		}
	}

	return received;
}


dc_status_t
shearwater_common_transfer (shearwater_common_device_t *device, const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize, unsigned int *actual)
{
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[SZ_PACKET + 4];
	int n = 0;

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
	n = shearwater_common_slip_write (device, packet, isize + 4);
	if (n != isize + 4) {
		ERROR (abstract->context, "Failed to send the request packet.");
		if (n < 0)
			return DC_STATUS_IO;
		else
			return DC_STATUS_TIMEOUT;
	}

	// Return early if no response packet is requested.
	if (osize == 0) {
		if (actual)
			*actual = 0;
		return DC_STATUS_SUCCESS;
	}

	// Receive the response packet.
	n = shearwater_common_slip_read (device, packet, sizeof (packet));
	if (n <= 0 || n > sizeof (packet)) {
		ERROR (abstract->context, "Failed to receive the response packet.");
		if (n < 0)
			return DC_STATUS_IO;
		else if (n > sizeof (packet))
			return DC_STATUS_PROTOCOL;
		else
			return DC_STATUS_TIMEOUT;
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
shearwater_common_download (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int address, unsigned int size, unsigned int compression)
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
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = 3 + size + 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

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
	progress.current += 3;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

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
		progress.current += length;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

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
	progress.current += 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_common_identifier (shearwater_common_device_t *device, dc_buffer_t *buffer, unsigned int id)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Erase the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Transfer the request.
	unsigned int n = 0;
	unsigned char request[] = {0x22,
		(id >> 8) & 0xFF,
		(id     ) & 0xFF};
	unsigned char response[SZ_PACKET];
	rc = shearwater_common_transfer (device, request, sizeof (request), response, sizeof (response), &n);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	// Verify the response.
	if (n < 3 || response[0] != 0x62 || response[1] != request[1] || response[2] != request[2]) {
		ERROR (abstract->context, "Unexpected response packet.");
		return DC_STATUS_PROTOCOL;
	}

	// Append the packet to the output buffer.
	if (!dc_buffer_append (buffer, response + 3, n - 3)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return rc;
}
