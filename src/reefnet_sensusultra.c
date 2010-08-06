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

#include "device-private.h"
#include "reefnet_sensusultra.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define PROMPT 0xA5
#define ACCEPT PROMPT
#define REJECT 0x00

typedef struct reefnet_sensusultra_device_t {
	device_t base;
	serial_t *port;
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE];
	unsigned int maxretries;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} reefnet_sensusultra_device_t;

static device_status_t reefnet_sensusultra_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t reefnet_sensusultra_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t reefnet_sensusultra_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t reefnet_sensusultra_device_close (device_t *abstract);

static const device_backend_t reefnet_sensusultra_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensusultra_device_dump, /* dump */
	reefnet_sensusultra_device_foreach, /* foreach */
	reefnet_sensusultra_device_close /* close */
};

static int
device_is_reefnet_sensusultra (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensusultra_device_backend;
}


device_status_t
reefnet_sensusultra_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t *) malloc (sizeof (reefnet_sensusultra_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &reefnet_sensusultra_device_backend);

	// Set the default values.
	device->port = NULL;
	device->maxretries = 2;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;
	memset (device->handshake, 0, sizeof (device->handshake));

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_close (device_t *abstract)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_get_handshake (device_t *abstract, unsigned char data[], unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_HANDSHAKE_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	memcpy (data, device->handshake, REEFNET_SENSUSULTRA_HANDSHAKE_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_set_maxretries (device_t *abstract, unsigned int maxretries)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->maxretries = maxretries;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != 4)
		return DEVICE_STATUS_ERROR;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_send_uchar (reefnet_sensusultra_device_t *device, unsigned char value)
{
	// Wait for the prompt byte.
	unsigned char prompt = 0;
	int rc = serial_read (device->port, &prompt, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the prompt byte");
		return EXITCODE (rc);
	}

	// Verify the prompt byte.
	if (prompt != PROMPT) {
		WARNING ("Unexpected answer data.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Send the value to the device.
	rc = serial_write (device->port, &value, 1);
	if (rc != 1) {
		WARNING ("Failed to send the value.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_send_ushort (reefnet_sensusultra_device_t *device, unsigned short value)
{
	// Send the least-significant byte.
	unsigned char lsb = value & 0xFF;
	device_status_t rc = reefnet_sensusultra_send_uchar (device, lsb);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the most-significant byte.
	unsigned char msb = (value >> 8) & 0xFF;
	rc = reefnet_sensusultra_send_uchar (device, msb);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_packet (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int header)
{
	assert (size >= header + 2);

	device_t *abstract = (device_t *) device;

	if (device_is_cancelled (abstract))
		return DEVICE_STATUS_CANCELLED;

	// Receive the data packet.
	int rc = serial_read (device->port, data, size);
	if (rc != size) {
		WARNING ("Failed to receive the packet.");
		return EXITCODE (rc);
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (data + size - 2);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (data + header, size - header - 2);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_handshake (reefnet_sensusultra_device_t *device, unsigned short value)
{
	// Wake-up the device.
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE + 2] = {0};
	device_status_t rc = reefnet_sensusultra_packet (device, handshake, sizeof (handshake), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (handshake + 4);

	// Store the handshake packet.
	memcpy (device->handshake, handshake, REEFNET_SENSUSULTRA_HANDSHAKE_SIZE);

	// Emit a clock event.
	device_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DEVICE_EVENT_CLOCK, &clock);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = handshake[1];
	devinfo.firmware = handshake[0];
	devinfo.serial = array_uint16_le (handshake + 2);
	device_event_emit (&device->base, DEVICE_EVENT_DEVINFO, &devinfo);

	// Send the instruction code to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_page (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int pagenum)
{
	assert (size >= REEFNET_SENSUSULTRA_PACKET_SIZE + 4);

	unsigned int nretries = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = reefnet_sensusultra_packet (device, data, size, 2)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet, 
		// and request a new one.
		if (rc != DEVICE_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= device->maxretries)
			return rc;

		// Reject the packet.
		rc = reefnet_sensusultra_send_uchar (device, REJECT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;
	}

	// Verify the page number.
	unsigned int page = array_uint16_le (data);
	if (page != pagenum) {
		WARNING ("Unexpected page number."); 
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_send (reefnet_sensusultra_device_t *device, unsigned short command)
{
	// Flush the input and output buffers.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Wake-up the device and send the instruction code.
	unsigned int nretries = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = reefnet_sensusultra_handshake (device, command)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted handshake packet,
		// and wait for the next one.
		if (rc != DEVICE_STATUS_PROTOCOL && rc != DEVICE_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= device->maxretries)
			return rc;

		// According to the developers guide, a 250 ms delay is suggested to
		// guarantee that the prompt byte sent after the handshake packet is
		// not accidentally buffered by the host and (mis)interpreted as part
		// of the next packet.

		serial_sleep (250);
		serial_flush (device->port, SERIAL_QUEUE_BOTH);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, 0xB421);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE) {
		// Receive the packet.
		unsigned char packet[REEFNET_SENSUSULTRA_PACKET_SIZE + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += REEFNET_SENSUSULTRA_PACKET_SIZE;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		// Prepend the packet to the buffer.
		if (!dc_buffer_prepend (buffer, packet + 2, REEFNET_SENSUSULTRA_PACKET_SIZE)) {
			WARNING ("Insufficient buffer space available.");
			return DEVICE_STATUS_MEMORY;
		}

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_read_user (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, 0xB420);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE) {
		// Receive the packet.
		unsigned char packet[REEFNET_SENSUSULTRA_PACKET_SIZE + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Append the packet to the buffer.
		memcpy (data + nbytes, packet + 2, REEFNET_SENSUSULTRA_PACKET_SIZE);

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_write_user (device_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = REEFNET_SENSUSULTRA_MEMORY_USER_SIZE + 2;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, 0xB430);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the data to the device.
	for (unsigned int i = 0; i < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE; ++i) {
		rc = reefnet_sensusultra_send_uchar (device, data[i]);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += 1;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);
	}

	// Send the checksum to the device.
	unsigned short crc = checksum_crc_ccitt_uint16 (data, REEFNET_SENSUSULTRA_MEMORY_USER_SIZE);
	rc = reefnet_sensusultra_send_ushort (device, crc);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Update and emit a progress event.
	progress.current += 2;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_write_parameter (device_t *abstract, reefnet_sensusultra_parameter_t parameter, unsigned int value)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Set the instruction code and validate the new value.
	unsigned short code = 0;
	switch (parameter) {
	case REEFNET_SENSUSULTRA_PARAMETER_INTERVAL:
		code = 0xB410;
		if (value < 1 || value > 65535)
			return DEVICE_STATUS_ERROR;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_THRESHOLD:
		code = 0xB411;
		if (value < 1 || value > 65535)
			return DEVICE_STATUS_ERROR;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_ENDCOUNT:
		code = 0xB412;
		if (value < 1 || value > 65535)
			return DEVICE_STATUS_ERROR;
		break;
	case REEFNET_SENSUSULTRA_PARAMETER_AVERAGING:
		code = 0xB413;
		if (value != 1 && value != 2 && value != 4)
			return DEVICE_STATUS_ERROR;
		break;
	default:
		return DEVICE_STATUS_ERROR;
	}

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, code);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the new value to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_sense (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_SENSE_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, 0xB440);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Receive the packet.
	unsigned char package[REEFNET_SENSUSULTRA_SENSE_SIZE + 2] = {0};
	rc = reefnet_sensusultra_packet (device, package, sizeof (package), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	memcpy (data, package, REEFNET_SENSUSULTRA_SENSE_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_parse (reefnet_sensusultra_device_t *device,
	const unsigned char data[], unsigned int *premaining, unsigned int *pprevious,
	int *aborted, dive_callback_t callback, void *userdata)
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
				return DEVICE_STATUS_SUCCESS;
			}

			if (callback && !callback (current, previous - current, current + 4, 4, userdata)) {
				if (aborted)
					*aborted = 1;
				return DEVICE_STATUS_SUCCESS;
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

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	dc_buffer_t *buffer = dc_buffer_new (REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE);
	if (buffer == NULL) {
		WARNING ("Memory allocation error.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wake-up the device and send the instruction code.
	device_status_t rc = reefnet_sensusultra_send (device, 0xB421);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Initialize the state for the incremental parser.
	unsigned int remaining = 0;
	unsigned int previous = 0;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE) {
		// Receive the packet.
		unsigned char packet[REEFNET_SENSUSULTRA_PACKET_SIZE + 4] = {0};
		rc = reefnet_sensusultra_page (device, packet, sizeof (packet), npages);
		if (rc != DEVICE_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}

		// Update and emit a progress event.
		progress.current += REEFNET_SENSUSULTRA_PACKET_SIZE;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		// Abort the transfer if the page contains no useful data.
		if (array_isequal (packet + 2, REEFNET_SENSUSULTRA_PACKET_SIZE, 0xFF) && nbytes != 0)
			break;

		// Prepend the packet to the buffer.
		if (!dc_buffer_prepend (buffer, packet + 2, REEFNET_SENSUSULTRA_PACKET_SIZE)) {
			WARNING ("Insufficient buffer space available.");
			return DEVICE_STATUS_MEMORY;
		}

		// Update the parser state.
		remaining += REEFNET_SENSUSULTRA_PACKET_SIZE;
		previous += REEFNET_SENSUSULTRA_PACKET_SIZE;

		// Parse the page data.
		int aborted = 0;
		rc = reefnet_sensusultra_parse (device, dc_buffer_get_data (buffer),
			&remaining, &previous, &aborted, callback, userdata);
		if (rc != DEVICE_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}
		if (aborted)
			break;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS) {
			dc_buffer_free (buffer);
			return rc;
		}

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	dc_buffer_free (buffer);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t *) abstract;

	if (abstract && !device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned int remaining = size;
	unsigned int previous = size;

	return reefnet_sensusultra_parse (device, data, &remaining, &previous, NULL, callback, userdata);
}
