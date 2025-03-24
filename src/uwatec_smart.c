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

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include "uwatec_smart.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "platform.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &uwatec_smart_device_vtable)

#define DATASIZE_RX 255
#define DATASIZE_TX 254
#define PACKETSIZE_USBHID_RX 64
#define PACKETSIZE_USBHID_TX 32

#define CMD_MODEL      0x10
#define CMD_HARDWARE   0x11
#define CMD_SOFTWARE   0x13
#define CMD_SERIAL     0x14
#define CMD_DEVTIME    0x1A
#define CMD_HANDSHAKE1 0x1B
#define CMD_HANDSHAKE2 0x1C
#define CMD_DATA       0xC4
#define CMD_SIZE       0xC6

#define OK  0x01
#define ACK 0x11
#define NAK 0x66

typedef struct uwatec_smart_device_t uwatec_smart_device_t;

typedef dc_status_t (*uwatec_smart_receive_t) (uwatec_smart_device_t *device, dc_event_progress_t *progress, unsigned char cmd, unsigned char data[], size_t size);
typedef dc_status_t (*uwatec_smart_send_t) (uwatec_smart_device_t *device, unsigned char cmd, const unsigned char data[], size_t size);

struct uwatec_smart_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	uwatec_smart_send_t send;
	uwatec_smart_receive_t receive;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
};

static dc_status_t uwatec_smart_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t uwatec_smart_device_vtable = {
	sizeof(uwatec_smart_device_t),
	DC_FAMILY_UWATEC_SMART,
	uwatec_smart_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	uwatec_smart_device_dump, /* dump */
	uwatec_smart_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};

static dc_status_t
uwatec_smart_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

static dc_status_t
uwatec_smart_irda_send (uwatec_smart_device_t *device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > DATASIZE_TX) {
		ERROR (abstract->context, "Command too large (" DC_PRINTF_SIZE ").", size);
		return DC_STATUS_PROTOCOL;
	}

	// Build the packet.
	unsigned char packet[1 + DATASIZE_TX] = {
		cmd};
	if (size) {
		memcpy (packet + 1, data, size);
	}

	// Send the packet.
	rc = dc_iostream_write (device->iostream, packet, size + 1, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the data packet.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
uwatec_smart_irda_receive (uwatec_smart_device_t *device, dc_event_progress_t *progress, unsigned char cmd, unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	size_t nbytes = 0;
	while (nbytes < size) {
		// Set the minimum packet size.
		size_t len = 32;

		// Increase the packet size if more data is immediately available.
		size_t available = 0;
		rc = dc_iostream_get_available (device->iostream, &available);
		if (rc == DC_STATUS_SUCCESS && available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > size)
			len = size - nbytes;

		rc = dc_iostream_read (device->iostream, data + nbytes, len, NULL);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the data packet.");
			return rc;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
uwatec_smart_serial_send (uwatec_smart_device_t *device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > DATASIZE_TX) {
		ERROR (abstract->context, "Command too large (" DC_PRINTF_SIZE ").", size);
		return DC_STATUS_PROTOCOL;
	}

	// Build the packet.
	unsigned char packet[12 + DATASIZE_TX + 1] = {
		0xFF, 0xFF, 0xFF,
		0xA6, 0x59, 0xBD, 0xC2,
		size + 1,
		0x00, 0x00, 0x00,
		cmd};
	if (size) {
		memcpy (packet + 12, data, size);
	}
	packet[12 + size] = checksum_xor_uint8 (packet + 7, size + 5, 0x00);

	// Send the packet.
	status = dc_iostream_write (device->iostream, packet, size + 13, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the echo and the ACK byte.
	unsigned char echo[sizeof(packet) + 1];
	status = dc_iostream_read (device->iostream, echo, size + 13 + 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the echo.");
		return status;
	}

	// Verify the echo.
	if (memcmp (echo, packet, size + 13) != 0) {
		WARNING (abstract->context, "Unexpected echo.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the ACK byte.
	unsigned char ack = echo[size + 13];
	if (ack != ACK) {
		WARNING (abstract->context, "Unexpected ACK byte (%02x).", ack);
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
uwatec_smart_serial_receive (uwatec_smart_device_t *device, dc_event_progress_t *progress, unsigned char cmd, unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	size_t nbytes = 0;
	while (nbytes < size) {
		// Read the header.
		unsigned char header[5];
		status = dc_iostream_read (device->iostream, header, sizeof (header), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the header.");
			return status;
		}

		// Get the packet size.
		unsigned int len = array_uint32_le (header);
		if (len < 1 || nbytes + len - 1 > size) {
			WARNING (abstract->context, "Unexpected header size (%u).", len);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the command byte.
		unsigned char rsp = header[4];
		if (rsp != cmd) {
			ERROR (abstract->context, "Unexpected header command byte (%02x).", rsp);
			return DC_STATUS_PROTOCOL;
		}

		// Read the packet data.
		status = dc_iostream_read (device->iostream, data + nbytes, len - 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return status;
		}

		// Read the checksum.
		unsigned char csum = 0x00;
		status = dc_iostream_read (device->iostream, &csum, sizeof (csum), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the checksum.");
			return status;
		}

		// Verify the checksum.
		unsigned char ccsum = 0x00;
		ccsum = checksum_xor_uint8 (header, sizeof (header), ccsum);
		ccsum = checksum_xor_uint8 (data + nbytes, len - 1, ccsum);
		if (csum != ccsum) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len - 1;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		nbytes += len - 1;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
uwatec_smart_usbhid_send (uwatec_smart_device_t *device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);
	unsigned char buf[DATASIZE_TX + 3];

	size_t packetsize = transport == DC_TRANSPORT_USBHID ?
		PACKETSIZE_USBHID_TX + 1 : sizeof(buf);

	if (size > DATASIZE_TX || size + 3 > packetsize) {
		ERROR (abstract->context, "Command too large (" DC_PRINTF_SIZE ").", size);
		return DC_STATUS_INVALIDARGS;
	}

	buf[0] = 0;
	buf[1] = size + 1;
	buf[2] = cmd;
	if (size) {
		memcpy(buf + 3, data, size);
	}
	memset(buf + 3 + size, 0, sizeof(buf) - (size + 3));

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "cmd", buf + 2, size + 1);

	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE) {
		rc = dc_iostream_write(device->iostream, buf + 1, size + 2, NULL);
	} else {
		rc = dc_iostream_write(device->iostream, buf, packetsize, NULL);
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
uwatec_smart_usbhid_receive (uwatec_smart_device_t *device, dc_event_progress_t *progress, unsigned char cmd, unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);
	unsigned char buf[DATASIZE_RX + 1];

	size_t packetsize = transport == DC_TRANSPORT_USBHID ?
		PACKETSIZE_USBHID_RX : sizeof(buf);

	size_t nbytes = 0;
	while (nbytes < size) {
		size_t transferred = 0;
		rc = dc_iostream_read (device->iostream, buf, packetsize, &transferred);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return rc;
		}

		if (transferred < 1) {
			ERROR (abstract->context, "Invalid packet length (" DC_PRINTF_SIZE ").", transferred);
			return DC_STATUS_PROTOCOL;
		}

		/*
		 * Something changed in the G2 firmware between versions 1.2 and 1.4.
		 *
		 * The first byte of a packet always used to be the length of the
		 * packet data. That's still true for simple single-packet replies,
		 * but multi-packet replies seem to have some other data in it, at
		 * least for BLE.
		 *
		 * The new pattern *seems* to be:
		 *
		 *   - simple one-packet reply: the byte remains the size of the reply
		 *
		 *   - otherwise, it's an endlessly repeating sequence of
		 *
		 *	0xf7 247
		 *	0x14  20
		 *	0x27  39
		 *	0x3a  58
		 *	0x4d  77
		 * 	0x60  96
		 *	0x73 115
		 *	0x86 134
		 *	0x99 153
		 *	0xac 172
		 *	0xbf 191
		 *	0xd2 210
		 *	0xe5 229
		 *	0xf7 247
		 *	.. repeats ..
		 *
		 * which is basically "increase by 19" except for that last one (229->247
		 * is an increase by 18).
		 *
		 * The number 19 is the real payload size for BLE GATT (20 bytes minus the
		 * one-byte magic size-that-isn't-size-any-more-byte).
		 *
		 * It may be just an oddly implemented sequence number. Whatever.
		 */
		unsigned int len = transferred - 1;
		if (transport == DC_TRANSPORT_USBHID) {
			if (len > buf[0])
				len = buf[0];
		}

		HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "rcv", buf + 1, len);

		if (len > size) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		memcpy(data + nbytes, buf + 1, len);
		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_transfer (uwatec_smart_device_t *device, unsigned char cmd, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	status = device->send (device, cmd, command, csize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	status = device->receive (device, NULL, cmd, answer, asize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_handshake (uwatec_smart_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;
	const unsigned char params[] = {0x10, 0x27, 0, 0};
	unsigned char answer[1] = {0};

	// Skip the handshake for BLE communication.
	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE)
		return DC_STATUS_SUCCESS;

	// Handshake (stage 1).
	dc_status_t rc = uwatec_smart_transfer (device, CMD_HANDSHAKE1, NULL, 0, answer, sizeof(answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != OK) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	rc = uwatec_smart_transfer (device, CMD_HANDSHAKE2, params, sizeof(params), answer, sizeof(answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != OK) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_smart_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_smart_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (uwatec_smart_device_t *) dc_device_allocate (context, &uwatec_smart_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Set the serial communication protocol (57600 8N1).
	status = dc_iostream_configure (device->iostream, 57600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (5000ms).
	status  = dc_iostream_set_timeout (device->iostream, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Select the correct send/receive function.
	dc_transport_t transport = dc_iostream_get_transport(iostream);
	switch (transport) {
	case DC_TRANSPORT_IRDA:
		device->send = uwatec_smart_irda_send;
		device->receive = uwatec_smart_irda_receive;
		break;
	case DC_TRANSPORT_SERIAL:
		device->send = uwatec_smart_serial_send;
		device->receive = uwatec_smart_serial_receive;
		break;
	case DC_TRANSPORT_USBHID:
	case DC_TRANSPORT_BLE:
		device->send = uwatec_smart_usbhid_send;
		device->receive = uwatec_smart_usbhid_receive;
		break;
	default:
		ERROR (context, "Unsupported transport type (%u).", transport);
		status = DC_STATUS_UNSUPPORTED;
		goto error_free;
	}

	// Perform the handshaking.
	status = uwatec_smart_handshake (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to handshake with the device.");
		goto error_free;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
uwatec_smart_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Read the model number.
	unsigned char model[1] = {0};
	rc = uwatec_smart_transfer (device, CMD_MODEL, NULL, 0, model, sizeof (model));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Model", model, sizeof (model));

	// Read the hardware version.
	unsigned char hardware[1] = {0};
	rc = uwatec_smart_transfer (device, CMD_HARDWARE, NULL, 0, hardware, sizeof (hardware));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Hardware", hardware, sizeof (hardware));

	// Read the software version.
	unsigned char software[1] = {0};
	rc = uwatec_smart_transfer (device, CMD_SOFTWARE, NULL, 0, software, sizeof (software));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Software", software, sizeof (software));

	// Read the serial number.
	unsigned char serial[4] = {0};
	rc = uwatec_smart_transfer (device, CMD_SERIAL, NULL, 0, serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Serial", serial, sizeof (serial));

	// Read the device clock.
	unsigned char devtime[4] = {0};
	rc = uwatec_smart_transfer (device, CMD_DEVTIME, NULL, 0, devtime, sizeof (devtime));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Clock", devtime, sizeof (devtime));

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (devtime);

	// Update and emit a progress event.
	progress.current += 11;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model[0];
	devinfo.firmware = bcd2dec (software[0]);
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Command parameters.
	const unsigned char params[] = {
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	unsigned char answer[4] = {0};
	rc = uwatec_smart_transfer (device, CMD_SIZE, params, sizeof (params), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 11 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

  	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	rc = uwatec_smart_transfer (device, CMD_DATA, params, sizeof (params), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	unsigned int total = array_uint32_le (answer);
	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	rc = device->receive (device, &progress, CMD_DATA, data, length);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_smart_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_smart_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
uwatec_smart_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
