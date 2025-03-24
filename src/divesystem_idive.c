/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Jef Driesen
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
#include <stdio.h>

#include "divesystem_idive.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"
#include "packet.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &divesystem_idive_device_vtable)

#define ISIX3M(model) ((model) >= 0x21)

#define MAXRETRIES 9

#define MAXPACKET 0xFF
#define START     0x55
#define ACK       0x06
#define WAIT      0x13
#define NAK       0x15

#define CMD_IDIVE_ID      0x10
#define CMD_IDIVE_RANGE   0x98
#define CMD_IDIVE_HEADER  0xA0
#define CMD_IDIVE_SAMPLE  0xA8

#define CMD_IX3M_ID       0x11
#define CMD_IX3M_RANGE    0x78
#define CMD_IX3M_HEADER   0x79
#define CMD_IX3M_SAMPLE   0x7A
#define CMD_IX3M_TIMESYNC 0x13
#define CMD_IX3M_BOOTLOADER 0x0A

#define BOOTLOADER_PROBE    0x78
#define BOOTLOADER_UPLOAD_A 0x40
#define BOOTLOADER_UPLOAD_B 0x23
#define BOOTLOADER_ACK      0x46

#define ERR_INVALID_CMD    0x10
#define ERR_INVALID_LENGTH 0x20
#define ERR_INVALID_DATA   0x30
#define ERR_UNSUPPORTED    0x40
#define ERR_UNAVAILABLE    0x58
#define ERR_UNREADABLE     0x5F
#define ERR_BUSY           0x60

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

#define EPOCH 1199145600 /* 2008-01-01 00:00:00 */

#define TZ_IDX_UNCHANGED 0xFF

typedef struct divesystem_idive_command_t {
	unsigned char cmd;
	unsigned int size;
} divesystem_idive_command_t;

typedef struct divesystem_idive_commands_t {
	divesystem_idive_command_t id;
	divesystem_idive_command_t range;
	divesystem_idive_command_t header;
	divesystem_idive_command_t sample;
	unsigned int nsamples;
} divesystem_idive_commands_t;

typedef struct divesystem_idive_signature_t {
	const char *name;
	unsigned int delay;
} divesystem_idive_signature_t;

typedef struct divesystem_idive_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[4];
	unsigned int model;
} divesystem_idive_device_t;

static dc_status_t divesystem_idive_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t divesystem_idive_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t divesystem_idive_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t divesystem_idive_device_close (dc_device_t *abstract);

static const dc_device_vtable_t divesystem_idive_device_vtable = {
	sizeof(divesystem_idive_device_t),
	DC_FAMILY_DIVESYSTEM_IDIVE,
	divesystem_idive_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	divesystem_idive_device_foreach, /* foreach */
	divesystem_idive_device_timesync, /* timesync */
	divesystem_idive_device_close /* close */
};

static const divesystem_idive_commands_t idive = {
	{CMD_IDIVE_ID,     0x0A},
	{CMD_IDIVE_RANGE,  0x04},
	{CMD_IDIVE_HEADER, 0x32},
	{CMD_IDIVE_SAMPLE, 0x2A},
	1,
};

static const divesystem_idive_commands_t ix3m = {
	{CMD_IX3M_ID,     0x1A},
	{CMD_IX3M_RANGE,  0x04},
	{CMD_IX3M_HEADER, 0x36},
	{CMD_IX3M_SAMPLE, 0x36},
	1,
};

static const divesystem_idive_commands_t ix3m_apos4 = {
	{CMD_IX3M_ID,     0x1A},
	{CMD_IX3M_RANGE,  0x04},
	{CMD_IX3M_HEADER, 0x36},
	{CMD_IX3M_SAMPLE, 0x40},
	3,
};

static const divesystem_idive_signature_t signatures[] = {
	{"dsh01", 50}, // IX3M GPS
	{"dsh30", 50}, // IX3M Pro
	{"dsh20",  5}, // iDive Sport
	{"dsh23",  5}, // iDive Color
	{"acx",    5}, // WPT
};

dc_status_t
divesystem_idive_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesystem_idive_device_t *device = NULL;
	dc_transport_t transport = dc_iostream_get_transport (iostream);

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (divesystem_idive_device_t *) dc_device_allocate (context, &divesystem_idive_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->model = model;

	// Create the packet stream.
	if (transport == DC_TRANSPORT_BLE) {
		status = dc_packet_open (&device->iostream, context, iostream, 244, 244);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to create the packet stream.");
			goto error_free;
		}
	} else {
		device->iostream = iostream;
	}

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free_iostream;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free_iostream;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free_iostream:
	if (transport == DC_TRANSPORT_BLE) {
		dc_iostream_close (device->iostream);
	}
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
divesystem_idive_device_close (dc_device_t *abstract)
{
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;

	// Close the packet stream.
	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE) {
		return dc_iostream_close (device->iostream);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesystem_idive_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_send (divesystem_idive_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET + 4];
	unsigned short crc = 0;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (csize < 1 || csize > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	packet[0] = START;
	packet[1] = csize;
	memcpy(packet + 2, command, csize);
	crc = checksum_crc16_ccitt (packet, csize + 2, 0xffff, 0x0000);
	packet[csize + 2] = (crc >> 8) & 0xFF;
	packet[csize + 3] = (crc     ) & 0xFF;

	// Send the data packet.
	status = dc_iostream_write (device->iostream, packet, csize + 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_receive (divesystem_idive_device_t *device, unsigned char answer[], unsigned int *asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET + 4];

	if (asize == NULL || *asize < MAXPACKET) {
		ERROR (abstract->context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Read the packet start byte.
	while (1) {
		status = dc_iostream_read (device->iostream, packet + 0, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet start byte.");
			return status;
		}

		if (packet[0] == START)
			break;
	}

	// Read the packet length.
	status = dc_iostream_read (device->iostream, packet + 1, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet length.");
		return status;
	}

	unsigned int len = packet[1];
	if (len < 2 || len > MAXPACKET) {
		ERROR (abstract->context, "Invalid packet length.");
		return DC_STATUS_PROTOCOL;
	}

	// Read the packet payload and checksum.
	status = dc_iostream_read (device->iostream, packet + 2, len + 2, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet payload and checksum.");
		return status;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be (packet + len + 2);
	unsigned short ccrc = checksum_crc16_ccitt (packet, len + 2, 0xffff, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected packet checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy(answer, packet + 2, len);
	*asize = len;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_packet (divesystem_idive_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET] = {0};
	unsigned int length = sizeof(packet);
	unsigned int errcode = 0;

	// Send the command.
	status = divesystem_idive_send (device, command, csize);
	if (status != DC_STATUS_SUCCESS) {
		goto error;
	}

	// Receive the answer.
	status = divesystem_idive_receive (device, packet, &length);
	if (status != DC_STATUS_SUCCESS) {
		goto error;
	}

	// Verify the command byte.
	if (packet[0] != command[0]) {
		ERROR (abstract->context, "Unexpected packet header.");
		status = DC_STATUS_PROTOCOL;
		goto error;
	}

	// Verify the ACK/NAK byte.
	unsigned int type = packet[length - 1];
	if (type != ACK && type != NAK) {
		ERROR (abstract->context, "Unexpected ACK/NAK byte.");
		status = DC_STATUS_PROTOCOL;
		goto error;
	}

	// Verify the length of the packet.
	unsigned int expected = (type == ACK ? asize : 1) + 2;
	if (length != expected) {
		ERROR (abstract->context, "Unexpected packet length.");
		status = DC_STATUS_PROTOCOL;
		goto error;
	}

	// Get the error code from a NAK packet.
	if (type == NAK) {
		errcode = packet[1];
		ERROR (abstract->context, "Received NAK packet with error code %02x.", errcode);
		status = DC_STATUS_PROTOCOL;
		goto error;
	}

	if (length > 2) {
		memcpy (answer, packet + 1, length - 2);
	}

error:
	if (errorcode) {
		*errorcode = errcode;
	}

	return status;
}


static dc_status_t
divesystem_idive_transfer (divesystem_idive_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int *errorcode)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned int errcode = 0;

	unsigned int nretries = 0;
	while ((status = divesystem_idive_packet (device, command, csize, answer, asize, &errcode)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (status != DC_STATUS_PROTOCOL && status != DC_STATUS_TIMEOUT)
			break;

		// Abort if the device reports a fatal error.
		if (errcode && errcode != ERR_BUSY)
			break;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			break;

		// Delay the next attempt.
		dc_iostream_sleep (device->iostream, 100);
	}

	if (errorcode) {
		*errorcode = errcode;
	}

	return status;
}

static dc_status_t
divesystem_idive_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;
	unsigned char packet[MAXPACKET - 2];
	unsigned int errcode = 0;

	const divesystem_idive_commands_t *commands = &idive;
	if (ISIX3M(device->model)) {
		commands = &ix3m;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	unsigned char cmd_id[] = {commands->id.cmd, 0xED};
	rc = divesystem_idive_transfer (device, cmd_id, sizeof(cmd_id), packet, commands->id.size, &errcode);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Version", packet, commands->id.size);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_le (packet);
	devinfo.firmware = array_uint32_le (packet + 2);
	devinfo.serial = array_uint32_le (packet + 6);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = packet;
	vendor.size = commands->id.size;
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	if (ISIX3M(device->model)) {
		// Detect the APOS4 firmware.
		unsigned int apos4 = (devinfo.firmware / 10000000) >= 4;
		if (apos4) {
			commands = &ix3m_apos4;
		}
	}

	unsigned char cmd_range[] = {commands->range.cmd, 0x8D};
	rc = divesystem_idive_transfer (device, cmd_range, sizeof(cmd_range), packet, commands->range.size, &errcode);
	if (rc != DC_STATUS_SUCCESS) {
		if (errcode == ERR_UNAVAILABLE) {
			return DC_STATUS_SUCCESS; // No dives found.
		} else {
			return rc;
		}
	}

	// Get the range of the available dive numbers.
	unsigned int first = array_uint16_le (packet + 0);
	unsigned int last  = array_uint16_le (packet + 2);
	if (first > last) {
		ERROR(abstract->context, "Invalid dive numbers.");
		return DC_STATUS_DATAFORMAT;
	}

	// Calculate the number of dives.
	unsigned int ndives = last - first + 1;

	// Update and emit a progress event.
	progress.maximum = ndives * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int number = last - i;
		unsigned char cmd_header[] = {commands->header.cmd,
			(number     ) & 0xFF,
			(number >> 8) & 0xFF};
		rc = divesystem_idive_transfer (device, cmd_header, sizeof(cmd_header), packet, commands->header.size, &errcode);
		if (rc != DC_STATUS_SUCCESS) {
			if (errcode == ERR_UNREADABLE) {
				WARNING(abstract->context, "Skipped unreadable dive!");
				continue;
			} else {
				dc_buffer_free(buffer);
				return rc;
			}
		}

		if (memcmp(packet + 7, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		unsigned int nsamples = array_uint16_le (packet + 1);

		// Update and emit a progress event.
		progress.current = i * NSTEPS + STEP(1, nsamples + 1);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		dc_buffer_clear(buffer);
		dc_buffer_reserve(buffer, commands->header.size + commands->sample.size * nsamples);

		if (!dc_buffer_append(buffer, packet, commands->header.size)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			dc_buffer_free(buffer);
			return DC_STATUS_NOMEMORY;
		}

		for (unsigned int j = 0; j < nsamples; j += commands->nsamples) {
			unsigned int idx = j + 1;
			unsigned char cmd_sample[] = {commands->sample.cmd,
				(idx     ) & 0xFF,
				(idx >> 8) & 0xFF};
			rc = divesystem_idive_transfer (device, cmd_sample, sizeof(cmd_sample), packet, commands->sample.size * commands->nsamples, &errcode);
			if (rc != DC_STATUS_SUCCESS) {
				dc_buffer_free(buffer);
				return rc;
			}

			// If the number of samples is not an exact multiple of the
			// number of samples per packet, then the last packet
			// appears to contain garbage data. Ignore those samples.
			unsigned int n = commands->nsamples;
			if (j + n > nsamples) {
				n = nsamples - j;
			}

			// Update and emit a progress event.
			progress.current = i * NSTEPS + STEP(j + n + 1, nsamples + 1);
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			if (!dc_buffer_append(buffer, packet, commands->sample.size * n)) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				dc_buffer_free(buffer);
				return DC_STATUS_NOMEMORY;
			}
		}

		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int   size = dc_buffer_get_size(buffer);
		if (callback && !callback (data, size, data + 7, sizeof(device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	dc_buffer_free(buffer);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesystem_idive_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;
	unsigned int errcode = 0;

	static const signed char tz_array[] = {
		-12,  0,    /* UTC-12    */
		-11,  0,    /* UTC-11    */
		-10,  0,    /* UTC-10    */
		 -9, 30,    /* UTC-9:30  */
		 -9,  0,    /* UTC-9     */
		 -8,  0,    /* UTC-8     */
		 -7,  0,    /* UTC-7     */
		 -6,  0,    /* UTC-6     */
		 -5,  0,    /* UTC-5     */
		 -4, 30,    /* UTC-4:30  */
		 -4,  0,    /* UTC-4     */
		 -3, 30,    /* UTC-3:30  */
		 -3,  0,    /* UTC-3     */
		 -2,  0,    /* UTC-2     */
		 -1,  0,    /* UTC-1     */
		  0,  0,    /* UTC       */
		  1,  0,    /* UTC+1     */
		  2,  0,    /* UTC+2     */
		  3,  0,    /* UTC+3     */
		  3, 30,    /* UTC+3:30  */
		  4,  0,    /* UTC+4     */
		  4, 30,    /* UTC+4:30  */
		  5,  0,    /* UTC+5     */
		  5, 30,    /* UTC+5:30  */
		  5, 45,    /* UTC+5:45  */
		  6,  0,    /* UTC+6     */
		  6, 30,    /* UTC+6:30  */
		  7,  0,    /* UTC+7     */
		  8,  0,    /* UTC+8     */
		  8, 45,    /* UTC+8:45  */
		  9,  0,    /* UTC+9     */
		  9, 30,    /* UTC+9:30  */
		  9, 45,    /* UTC+9:45  */
		 10,  0,    /* UTC+10    */
		 10, 30,    /* UTC+10:30 */
		 11,  0,    /* UTC+11    */
		 11, 30,    /* UTC+11:30 */
		 12,  0,    /* UTC+12    */
		 12, 45,    /* UTC+12:45 */
		 13,  0,    /* UTC+13    */
		 13, 45,    /* UTC+13:45 */
		 14,  0     /* UTC+14    */
	};

	if (!ISIX3M(device->model)) {
		return DC_STATUS_UNSUPPORTED;
	}

	// Get the UTC timestamp.
	dc_ticks_t timestamp = dc_datetime_mktime(datetime);
	if (timestamp == -1) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Adjust the epoch.
	timestamp -= EPOCH;

	// Find the timezone index.
	size_t tz_idx = C_ARRAY_SIZE(tz_array);
	for (size_t i = 0; i < C_ARRAY_SIZE(tz_array); i += 2) {
		int timezone = tz_array[i] * 3600;
		if (timezone < 0) {
			timezone -= tz_array[i + 1] * 60;
		} else {
			timezone += tz_array[i + 1] * 60;
		}

		if (timezone == datetime->timezone) {
			tz_idx = i;
			break;
		}
	}
	if (tz_idx >= C_ARRAY_SIZE(tz_array)) {
		ERROR (abstract->context, "Invalid timezone value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Adjust the timezone index.
	tz_idx /= 2;

	// Send the command.
	unsigned char command[] = {
		CMD_IX3M_TIMESYNC,
		(timestamp >>  0) & 0xFF,
		(timestamp >>  8) & 0xFF,
		(timestamp >> 16) & 0xFF,
		(timestamp >> 24) & 0xFF,
		tz_idx,            // Home timezone
		TZ_IDX_UNCHANGED}; // Travel timezone
	rc = divesystem_idive_transfer (device, command, sizeof(command), NULL, 0, &errcode);
	if (rc != DC_STATUS_SUCCESS) {
		if (errcode == ERR_INVALID_LENGTH || errcode == ERR_INVALID_DATA) {
			// Fallback to the variant without the second timezone if the
			// firmware doesn't support two timezones (ERR_INVALID_LENGTH) or
			// leaving the timezone unchanged (ERR_INVALID_DATA).
			rc = divesystem_idive_transfer (device, command, sizeof(command) - 1, NULL, 0, &errcode);
			if (rc != DC_STATUS_SUCCESS) {
				return rc;
			}
		} else {
			return rc;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesystem_idive_firmware_readfile (dc_buffer_t *buffer, dc_context_t *context, const char *filename)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *tmp = NULL;
	FILE *fp = NULL;

	if (!dc_buffer_clear (buffer)) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Allocate a temporary buffer.
	tmp = dc_buffer_new (0x20000);
	if (tmp == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Open the file.
	fp = fopen (filename, "rb");
	if (fp == NULL) {
		ERROR (context, "Failed to open the file.");
		status = DC_STATUS_IO;
		goto error_free;
	}

	// Read the entire file into the buffer.
	size_t n = 0;
	unsigned char block[4096] = {0};
	while ((n = fread (block, 1, sizeof (block), fp)) > 0) {
		if (!dc_buffer_append (tmp, block, n)) {
			ERROR (context, "Insufficient buffer space available.");
			status = DC_STATUS_NOMEMORY;
			goto error_close;
		}
	}

	// Resize the output buffer.
	size_t nbytes = dc_buffer_get_size (tmp);
	if (!dc_buffer_resize (buffer, nbytes / 2)) {
		ERROR (context, "Insufficient buffer space available.");
		status = DC_STATUS_NOMEMORY;
		goto error_close;
	}

	// Convert to binary data.
	int rc = array_convert_hex2bin (
		dc_buffer_get_data (tmp), dc_buffer_get_size (tmp),
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer));
	if (rc != 0) {
		ERROR (context, "Unexpected data format.");
		status = DC_STATUS_DATAFORMAT;
		goto error_close;
	}

error_close:
	fclose (fp);
error_free:
	dc_buffer_free (tmp);
error_exit:
	return status;
}

static dc_status_t
divesystem_idive_firmware_send (divesystem_idive_device_t *device, const divesystem_idive_signature_t *signature, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	unsigned int nretries = 0;
	while (1) {
		// Send the frame.
		status = dc_iostream_write (device->iostream, data, size, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the frame.");
			return status;
		}

		// Read the response until an ACK or NAK byte is received.
		unsigned int state = 0;
		while (state == 0) {
			// Receive the response.
			unsigned char response = 0;
			status = dc_iostream_read (device->iostream, &response, 1, NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the response.");
				return status;
			}

			// Process the response.
			switch (response) {
			case ACK:
			case NAK:
				state = response;
				break;
			case WAIT:
				dc_iostream_sleep (device->iostream, signature->delay);
				break;
			case 'A':
			case 'B':
			case 'C':
			case 'D':
			case 'E':
			case 'F':
			case 'G':
			case 'H':
			case 'K':
			case 'X':
				break;
			default:
				WARNING (abstract->context, "Unexpected response byte received (%02x)", response);
				break;
			}
		}

		// Exit if ACK received.
		if (state == ACK)
			break;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES) {
			ERROR (abstract->context, "Maximum number of retries reached.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
divesystem_idive_device_fwupdate (dc_device_t *abstract, const char *filename)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;
	unsigned int errcode = 0;

	// Allocate memory for the firmware data.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for the firmware data.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Read the firmware file.
	status = divesystem_idive_firmware_readfile (buffer, abstract->context, filename);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the firmware file.");
		goto error_free;
	}

	// Cache the data and size.
	const unsigned char *data = dc_buffer_get_data (buffer);
	size_t size = dc_buffer_get_size (buffer);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Activate the bootloader.
	const unsigned char bootloader[] = {CMD_IX3M_BOOTLOADER, 0xC9, 0x4B};
	status = divesystem_idive_transfer (device, bootloader, sizeof (bootloader), NULL, 0, &errcode);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to activate the bootloader.");
		goto error_free;
	}

	// Give the device some time to enter the bootloader.
	dc_iostream_sleep (device->iostream, 2000);

	// Wait for the bootloader.
	const divesystem_idive_signature_t *signature = NULL;
	while (signature == NULL) {
		// Discard garbage data.
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);

		// Probe for the bootloader.
		const unsigned char probe[] = {BOOTLOADER_PROBE};
		status = dc_iostream_write (device->iostream, probe, sizeof (probe), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to activate the bootloader.");
			goto error_free;
		}

		// Read the signature string.
		size_t n = 0;
		unsigned char name[5] = {0};
		status = dc_iostream_read (device->iostream, name, sizeof (name), &n);
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_TIMEOUT) {
			ERROR (abstract->context, "Failed to read the signature string.");
			goto error_free;
		}

		// Verify the signature string.
		for (size_t i = 0; i < C_ARRAY_SIZE (signatures); ++i) {
			if (n == strlen (signatures[i].name) && memcmp (name, signatures[i].name, n) == 0) {
				signature = signatures + i;
				break;
			}
		}
	}

	// Send the start upload command.
	const unsigned char upload[] = {BOOTLOADER_UPLOAD_A, BOOTLOADER_UPLOAD_B};
	status = dc_iostream_write (device->iostream, upload, sizeof(upload), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the start upload command.");
		goto error_free;
	}

	// Receive the ack.
	unsigned char ack[1] = {0};
	status = dc_iostream_read (device->iostream, ack, sizeof(ack), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the ack byte.");
		goto error_free;
	}

	// Verify the ack.
	if (ack[0] != BOOTLOADER_ACK) {
		ERROR (abstract->context, "Invalid ack byte (%02x).", ack[0]);
		status = DC_STATUS_PROTOCOL;
		goto error_free;
	}

	// Wait before sending the firmware data.
	dc_iostream_sleep (device->iostream, 100);

	// Upload the firmware.
	unsigned int offset = 0;
	while (offset + 2 <= size) {
		// Get the number of bytes in the current frame.
		unsigned int len = array_uint16_be (data + offset) + 2;
		if (offset + len > size) {
			ERROR (abstract->context, "Invalid frame size (%u %u " DC_PRINTF_SIZE ")", offset, len, size);
			status = DC_STATUS_DATAFORMAT;
			goto error_free;
		}

		// Send the frame.
		status = divesystem_idive_firmware_send (device, signature, data + offset, len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the frame.");
			goto error_free;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		offset += len;
	}

error_free:
	dc_buffer_free (buffer);
error_exit:
	return status;
}
