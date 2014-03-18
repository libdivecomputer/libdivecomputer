/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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
#include <assert.h> // assert

#include <libdivecomputer/reefnet_sensusultra.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &reefnet_sensusultra_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define SZ_PACKET    512
#define SZ_MEMORY    2080768
#define SZ_USER      16384
#define SZ_HANDSHAKE 24
#define SZ_SENSE     6

#define MAXRETRIES 2
#define PROMPT 0xA5
#define ACCEPT PROMPT
#define REJECT 0x00

typedef struct reefnet_sensusultra_device_t {
	dc_device_t base;
	serial_t *port;
	unsigned char handshake[SZ_HANDSHAKE];
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} reefnet_sensusultra_device_t;

static dc_status_t reefnet_sensusultra_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t reefnet_sensusultra_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t reefnet_sensusultra_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t reefnet_sensusultra_device_close (dc_device_t *abstract);

static const dc_device_vtable_t reefnet_sensusultra_device_vtable = {
	DC_FAMILY_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensusultra_device_dump, /* dump */
	reefnet_sensusultra_device_foreach, /* foreach */
	reefnet_sensusultra_device_close /* close */
};


dc_status_t
reefnet_sensusultra_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t *) malloc (sizeof (reefnet_sensusultra_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &reefnet_sensusultra_device_vtable);

	// Set the default values.
	device->port = NULL;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;
	memset (device->handshake, 0, sizeof (device->handshake));

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_device_close (dc_device_t *abstract)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_device_get_handshake (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_HANDSHAKE) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	memcpy (data, device->handshake, SZ_HANDSHAKE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_send_uchar (reefnet_sensusultra_device_t *device, unsigned char value)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Wait for the prompt byte.
	unsigned char prompt = 0;
	int rc = serial_read (device->port, &prompt, 1);
	if (rc != 1) {
		ERROR (abstract->context, "Failed to receive the prompt byte");
		return EXITCODE (rc);
	}

	// Verify the prompt byte.
	if (prompt != PROMPT) {
		ERROR (abstract->context, "Unexpected answer data.");
		return DC_STATUS_PROTOCOL;
	}

	// Send the value to the device.
	rc = serial_write (device->port, &value, 1);
	if (rc != 1) {
		ERROR (abstract->context, "Failed to send the value.");
		return EXITCODE (rc);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_send_ushort (reefnet_sensusultra_device_t *device, unsigned short value)
{
	// Send the least-significant byte.
	unsigned char lsb = value & 0xFF;
	dc_status_t rc = reefnet_sensusultra_send_uchar (device, lsb);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the most-significant byte.
	unsigned char msb = (value >> 8) & 0xFF;
	rc = reefnet_sensusultra_send_uchar (device, msb);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_packet (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int header)
{
	assert (size >= header + 2);

	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Receive the data packet.
	int rc = serial_read (device->port, data, size);
	if (rc != size) {
		ERROR (abstract->context, "Failed to receive the packet.");
		return EXITCODE (rc);
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (data + size - 2);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (data + header, size - header - 2);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_handshake (reefnet_sensusultra_device_t *device, unsigned short value)
{
	// Wake-up the device.
	unsigned char handshake[SZ_HANDSHAKE + 2] = {0};
	dc_status_t rc = reefnet_sensusultra_packet (device, handshake, sizeof (handshake), 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (handshake + 4);

	// Store the handshake packet.
	memcpy (device->handshake, handshake, SZ_HANDSHAKE);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = handshake[1];
	devinfo.firmware = handshake[0];
	devinfo.serial = array_uint16_le (handshake + 2);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->handshake;
	vendor.size = sizeof (device->handshake);
	device_event_emit (&device->base, DC_EVENT_VENDOR, &vendor);

	// Send the instruction code to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_page (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int pagenum)
{
	dc_device_t *abstract = (dc_device_t *) device;

	assert (size >= SZ_PACKET + 4);

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = reefnet_sensusultra_packet (device, data, size, 2)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Reject the packet.
		rc = reefnet_sensusultra_send_uchar (device, REJECT);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	// Verify the page number.
	unsigned int page = array_uint16_le (data);
	if (page != pagenum) {
		ERROR (abstract->context, "Unexpected page number.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_send (reefnet_sensusultra_device_t *device, unsigned short command)
{
	// Flush the input and output buffers.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Wake-up the device and send the instruction code.
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = reefnet_sensusultra_handshake (device, command)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted handshake packet,
		// and wait for the next one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// According to the developers guide, a 250 ms delay is suggested to
		// guarantee that the prompt byte sent after the handshake packet is
		// not accidentally buffered by the host and (mis)interpreted as part
		// of the next packet.

		serial_sleep (device->port, 250);
		serial_flush (device->port, SERIAL_QUEUE_BOTH);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, 0xB421);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < SZ_MEMORY) {
		// Receive the packet.
		unsigned char packet[SZ_PACKET + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += SZ_PACKET;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		// Prepend the packet to the buffer.
		if (!dc_buffer_prepend (buffer, packet + 2, SZ_PACKET)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += SZ_PACKET;
		npages++;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_device_read_user (dc_device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_USER) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, 0xB420);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < SZ_USER) {
		// Receive the packet.
		unsigned char packet[SZ_PACKET + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Append the packet to the buffer.
		memcpy (data + nbytes, packet + 2, SZ_PACKET);

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += SZ_PACKET;
		npages++;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_device_write_user (dc_device_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_USER) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_USER + 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, 0xB430);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the data to the device.
	for (unsigned int i = 0; i < SZ_USER; ++i) {
		rc = reefnet_sensusultra_send_uchar (device, data[i]);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += 1;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	// Send the checksum to the device.
	unsigned short crc = checksum_crc_ccitt_uint16 (data, SZ_USER);
	rc = reefnet_sensusultra_send_ushort (device, crc);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Update and emit a progress event.
	progress.current += 2;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_device_write_parameter (dc_device_t *abstract, reefnet_sensusultra_parameter_t parameter, unsigned int value)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Set the instruction code and validate the new value.
	unsigned short code = 0;
	switch (parameter) {
	case REEFNET_SENSUSULTRA_PARAMETER_INTERVAL:
		code = 0xB410;
		if (value < 1 || value > 65535)
			return DC_STATUS_INVALIDARGS;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_THRESHOLD:
		code = 0xB411;
		if (value < 1 || value > 65535)
			return DC_STATUS_INVALIDARGS;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_ENDCOUNT:
		code = 0xB412;
		if (value < 1 || value > 65535)
			return DC_STATUS_INVALIDARGS;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_AVERAGING:
		code = 0xB413;
		if (value != 1 && value != 2 && value != 4)
			return DC_STATUS_INVALIDARGS;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, code);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the new value to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_device_sense (dc_device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_SENSE) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, 0xB440);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Receive the packet.
	unsigned char package[SZ_SENSE + 2] = {0};
	rc = reefnet_sensusultra_packet (device, package, sizeof (package), 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	memcpy (data, package, SZ_SENSE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parse (reefnet_sensusultra_device_t *device,
	const unsigned char data[], unsigned int *premaining, unsigned int *pprevious,
	int *aborted, dc_dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	// Initialize the data stream pointers.
	const unsigned char *current  = data + *premaining;
	const unsigned char *previous = data + *pprevious;

	// Search the data stream for header markers.
	while ((current = array_search_backward (data, current - data, header, sizeof (header))) != NULL) {
		// Move the pointer to the begin of the header.
		current -= sizeof (header);

		// If there is a sequence of more than 4 zero bytes present, the header
		// marker is located at the start of this sequence, not the end.
		while (current > data && current[-1] == 0x00)
			current--;

		// Once a header marker is found, start searching
		// for the corresponding footer marker. The search is
		// now limited to the start of the previous dive.
		if (previous - current >= 16) {
			previous = array_search_forward (current + 16, previous - current - 16, footer, sizeof (footer));
		} else {
			previous = NULL;
		}

		// Skip dives without a footer marker.
		if (previous) {
			// Move the pointer to the end of the footer.
			previous += sizeof (footer);

			// Automatically abort when a dive is older than the provided timestamp.
			unsigned int timestamp = array_uint32_le (current + 4);
			if (device && timestamp <= device->timestamp) {
				if (aborted)
					*aborted = 1;
				return DC_STATUS_SUCCESS;
			}

			if (callback && !callback (current, previous - current, current + 4, 4, userdata)) {
				if (aborted)
					*aborted = 1;
				return DC_STATUS_SUCCESS;
			}
		}

		// Prepare for the next iteration.
		previous = current;

		// Return the current state.
		*premaining = *pprevious = current - data;
	}

	// Return the current state.
	*premaining = sizeof (header) - 1;
	if (*premaining > *pprevious)
		*premaining = *pprevious;

	if (aborted)
		*aborted = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	dc_status_t rc = reefnet_sensusultra_send (device, 0xB421);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Initialize the state for the incremental parser.
	unsigned int remaining = 0;
	unsigned int previous = 0;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < SZ_MEMORY) {
		// Receive the packet.
		unsigned char packet[SZ_PACKET + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DC_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}

		// Update and emit a progress event.
		progress.current += SZ_PACKET;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		// Abort the transfer if the page contains no useful data.
		if (array_isequal (packet + 2, SZ_PACKET, 0xFF) && nbytes != 0)
			break;

		// Prepend the packet to the buffer.
		if (!dc_buffer_prepend (buffer, packet + 2, SZ_PACKET)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		// Update the parser state.
		remaining += SZ_PACKET;
		previous += SZ_PACKET;

		// Parse the page data.
		int aborted = 0;
		rc = reefnet_sensusultra_parse (device, dc_buffer_get_data (buffer),
			&remaining, &previous, &aborted, callback, userdata);
		if (rc != DC_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}
		if (aborted)
			break;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DC_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}

		nbytes += SZ_PACKET;
		npages++;
	}

	dc_buffer_free (buffer);

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t *) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	unsigned int remaining = size;
	unsigned int previous = size;

	return reefnet_sensusultra_parse (device, data, &remaining, &previous, NULL, callback, userdata);
}
