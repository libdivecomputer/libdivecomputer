/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Linus Torvalds
 * Copyright (C) 2022 Jef Driesen
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
#include <stdarg.h>
#include <stdio.h>

#include "oceans_s1.h"
#include "oceans_s1_common.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

#define SOH 0x01
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define CRC 0x43

#define SZ_PACKET 256
#define SZ_XMODEM 512

#define SZ_FINGERPRINT 8

typedef struct oceans_s1_dive_t {
	struct oceans_s1_dive_t *next;
	dc_ticks_t timestamp;
	unsigned int number;
} oceans_s1_dive_t;

typedef struct oceans_s1_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	dc_ticks_t timestamp;
} oceans_s1_device_t;

static dc_status_t oceans_s1_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t oceans_s1_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t oceans_s1_device_vtable = {
	sizeof(oceans_s1_device_t),
	DC_FAMILY_OCEANS_S1,
	oceans_s1_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	oceans_s1_device_foreach, /* foreach */
	oceans_s1_device_timesync, /* timesync */
	NULL, /* close */
};

/*
 * Oceans S1 initial sequence (all ASCII text with newlines):
 *
 *    Cmd               Reply
 *
 *    utc\n             utc>ok 1592912375\r\n
 *    battery\n         battery>ok 59%\r\n
 *    version\n         version>ok 1.1 42a7e564\r\n
 *    utc 1592912364\n  utc>ok\r\n
 *    units 1\n         units>ok\r\n
 *    dllist\n          dllist>xmr\r\n
 *
 * At this point, the dive computer switches to the XMODEM protocol and
 * the sequence is no longer single packets with a full line with newline
 * termination.
 *
 * The actual payload remains ASCII text (note the single space indentation):
 *
 *    divelog v1,10s/sample
 *     dive 1,0,21,1591372057
 *     continue 612,10
 *     enddive 3131,496
 *     dive 2,0,21,1591372925
 *     enddive 1535,277
 *     dive 3,0,32,1591463368
 *     enddive 1711,4515
 *     dive 4,0,32,1591961688
 *     continue 300,45
 *     continue 391,238
 *     continue 420,126
 *     continue 236,17
 *     enddive 1087,2636
 *    endlog
 *
 * Because the XMODEM protocol uses fixed size packets (512 bytes), the last
 * packet is padded with newline characters.
 *
 * Then it goes back to line-mode:
 *
 *    dlget 4 5\n       dlget>xmr\r\n
 *
 * and the data is again transferred using the XMODEM protocol. The payload is
 * also ASCII text (note the space indentation again):
 *
 *    divelog v1,10s/sample
 *     dive 4,0,32,1591961688
 *      365,13,1
 *      382,13,51456
 *      367,13,16640
 *      381,13,49408
 *      375,13,24576
 *      355,13,16384
 *      346,13,16384
 *      326,14,16384
 *      355,14,16384
 *      394,14,24576
 *      397,14,16384
 *      434,14,49152
 *      479,14,49152
 *      488,14,16384
 *      556,14,57344
 *      616,14,49152
 *      655,14,49152
 *      738,14,49152
 *      800,14,57344
 *      800,14,49408
 *      834,14,16640
 *      871,14,24832
 *      860,14,16640
 *      860,14,16640
 *      815,14,24832
 *      738,14,16640
 *      707,14,16640
 *      653,14,24832
 *      647,13,16640
 *      670,13,16640
 *      653,13,24832
 *      ...
 *     continue 236,17
 *      227,13,57600
 *      238,14,16640
 *      267,14,24832
 *      283,14,16384
 *      272,14,16384
 *      303,14,24576
 *      320,14,16384
 *      318,14,16384
 *      318,14,16384
 *      335,14,24576
 *      332,14,16384
 *      386,14,16384
 *      417,14,24576
 *      244,14,16640
 *      71,14,16640
 *     enddive 1087,2636
 *    endlog
 *
 * Where the samples seem to be
 *  - depth in cm
 *  - temperature in °C
 *  - events
 *
 * Repeat with 'dlget 3 4', 'dlget 2 3', 'dlget 1 2'.
 *
 * Done.
 */

/*
 * Add a dive to the dive list, sorted with newest dive first
 *
 * I'm not sure if the dive list is always presented sorted by the
 * Oceans S1, but it arrives in the reverse order of what we want
 * (we want newest first, it lists them oldest first). So we need
 * to switch the order, and we might as well make sure it's sorted
 * while doing that.
 */
static void
oceans_s1_list_add (oceans_s1_dive_t **head, oceans_s1_dive_t *dive)
{
	if (head == NULL)
		return;

	oceans_s1_dive_t *current = *head, *previous = NULL;
	while (current) {
		if (dive->number >= current->number)
			break;
		previous = current;
		current = current->next;
	}

	if (previous) {
		dive->next = previous->next;
		previous->next = dive;
	} else {
		dive->next = *head;
		*head = dive;
	}
}

static void
oceans_s1_list_free (oceans_s1_dive_t *head)
{
	oceans_s1_dive_t *current = head;
	while (current) {
		oceans_s1_dive_t *next = current->next;
		free (current);
		current = next;
	}
}

/*
 * The main data is transferred using the XMODEM-CRC protocol.
 *
 * This variant of the XMODEM protocol uses a sequence of 517 byte packets,
 * where each packet has a three byte header, 512 bytes of payload data and a
 * two byte CRC checksum. The header is a 'SOH' byte, followed by the block
 * number (starting at 1), and the inverse block number (255-block).
 *
 * We're supposed to start the sequence with a 'CRC' byte, and reply to each
 * packet with a 'ACK' byte. When there is no more data, the device will
 * send us a 'EOT' packet, which we'll ack with a final 'ACK' byte.
 *
 * So we get a sequence of:
 *
 *  01 01 fe <512 bytes> xx xx
 *  01 02 fd <512 bytes> xx xx
 *  01 03 fc <512 bytes> xx xx
 *  01 04 fb <512 bytes> xx xx
 *  01 05 fa <512 bytes> xx xx
 *  01 06 f9 <512 bytes> xx xx
 *  01 07 f8 <512 bytes> xx xx
 *  04
 *
 * And we should reply with an 'ACK' byte for each of those entries.
 *
 * NOTE! The above is not in single BLE packets, although the
 * sequence blocks always start at a packet boundary.
 *
 * NOTE! The Oceans Android app uses GATT "Write Commands" (0x53), and not
 * GATT "Write Requests" (0x12) for sending the XMODEM single byte commands,
 * but this difference does not seem to matter.
 */
static dc_status_t
oceans_s1_xmodem_packet (oceans_s1_device_t *device, unsigned char seq, unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char packet[3 + SZ_XMODEM + 2] = {0};
	size_t nbytes = 0;

	if (size < SZ_XMODEM)
		return DC_STATUS_INVALIDARGS;

	status = dc_iostream_read (device->iostream, packet, sizeof(packet), &nbytes);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to receive the packet.");
		return status;
	}

	if (nbytes < 1) {
		ERROR (device->base.context, "Unexpected packet length (" DC_PRINTF_SIZE ").", nbytes);
		return DC_STATUS_PROTOCOL;
	}

	if (packet[0] == EOT) {
		return DC_STATUS_DONE;
	}

	if (nbytes < 3) {
		ERROR (device->base.context, "Unexpected packet length (" DC_PRINTF_SIZE ").", nbytes);
		return DC_STATUS_PROTOCOL;
	}

	if (packet[0] != SOH || packet[1] != seq || packet[1] + packet[2] != 0xFF) {
		ERROR (device->base.context, "Unexpected packet header.");
		return DC_STATUS_PROTOCOL;
	}

	while (nbytes < sizeof(packet)) {
		size_t received = 0;
		status = dc_iostream_read (device->iostream, packet + nbytes, sizeof(packet) - nbytes, &received);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (device->base.context, "Failed to receive the packet.");
			return status;
		}

		nbytes += received;
	}

	unsigned short crc = array_uint16_be (packet + nbytes - 2);
	unsigned short ccrc = checksum_crc16_ccitt (packet + 3, nbytes - 5, 0x0000, 0x0000);
	if (crc != ccrc) {
		ERROR (device->base.context, "Unexpected answer checksum (%04x %04x).", crc, ccrc);
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, packet + 3, SZ_XMODEM);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_xmodem_recv (oceans_s1_device_t *device, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	const unsigned char crc = CRC;
	const unsigned char ack = ACK;

	dc_buffer_clear (buffer);

	// Request XMODEM-CRC mode.
	status = dc_iostream_write (device->iostream, &crc, 1, NULL);
	if (status != DC_STATUS_SUCCESS)
		return status;

	unsigned char seq = 1;
	while (1) {
		// Receive the XMODEM data packet.
		unsigned char packet[SZ_XMODEM] = {0};
		status = oceans_s1_xmodem_packet (device, seq, packet, sizeof(packet));
		if (status != DC_STATUS_SUCCESS) {
			if (status == DC_STATUS_DONE)
				break;
			return status;
		}

		dc_buffer_append (buffer, packet, sizeof(packet));

		// Ack the data packet.
		status = dc_iostream_write (device->iostream, &ack, 1, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;

		seq++;
	}

	// Ack the EOT packet.
	status = dc_iostream_write (device->iostream, &ack, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Find trailing newline(s).
	size_t size = dc_buffer_get_size (buffer);
	unsigned char *data = dc_buffer_get_data (buffer);
	while (size > 1 && (data[size - 2] == '\r' || data[size - 2] == '\n'))
		size--;

	// Remove trailing newline(s).
	dc_buffer_slice (buffer, 0, size);

	return DC_STATUS_SUCCESS;
}

static dc_status_t DC_ATTR_FORMAT_PRINTF(6, 7)
oceans_s1_transfer (oceans_s1_device_t *device, dc_buffer_t *buffer, char data[], size_t size, const char *cmd, const char *params, ...)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	char buf[SZ_PACKET + 1] = {0};
	size_t buflen = 0;

	if (device_is_cancelled (&device->base))
		return DC_STATUS_CANCELLED;

	size_t cmdlen = strlen (cmd);
	if (buflen + cmdlen > sizeof(buf) - 1) {
		ERROR (device->base.context, "Not enough space for the command string.");
		return DC_STATUS_NOMEMORY;
	}

	// Copy the command string.
	memcpy (buf, cmd, cmdlen);
	buflen += cmdlen;

	// Null terminate the buffer.
	buf[buflen] = 0;

	if (params) {
		if (buflen + 1 > sizeof(buf) - 1) {
			ERROR (device->base.context, "Not enough space for the separator.");
			return DC_STATUS_NOMEMORY;
		}

		// Append a space.
		buf[buflen++] = ' ';

		// Null terminate the buffer.
		buf[buflen] = 0;

		// Append the arguments.
		va_list ap;
		va_start (ap, params);
		int n = dc_platform_vsnprintf (buf + buflen, sizeof(buf) - buflen, params, ap);
		va_end (ap);
		if (n < 0) {
			ERROR (device->base.context, "Not enough space for the arguments.");
			return DC_STATUS_NOMEMORY;
		}

		buflen += n;
	}

	DEBUG(device->base.context, "cmd: %s", buf);

	if (buflen + 1 > sizeof(buf) - 1) {
		ERROR (device->base.context, "Not enough space for the newline.");
		return DC_STATUS_NOMEMORY;
	}

	// Append a newline.
	buf[buflen++] = '\n';

	// Null terminate the buffer.
	buf[buflen] = 0;

	// Send the command.
	status = dc_iostream_write (device->iostream, buf, buflen, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send the command.");
		return status;
	}

	// Receive the response.
	size_t nbytes = 0;
	status = dc_iostream_read (device->iostream, buf, sizeof(buf) - 1, &nbytes);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to receive the response.");
		return status;
	}

	// Remove trailing newline(s).
	while (nbytes && (buf[nbytes - 1] == '\r' || buf[nbytes - 1] == '\n'))
		nbytes--;

	// Null terminate the buffer.
	buf[nbytes] = 0;

	DEBUG (device->base.context, "rcv: %s", buf);

	// Verify the response.
	if (strncmp (buf, cmd, cmdlen) != 0) {
		ERROR (device->base.context, "Received unexpected packet data ('%s').", buf);
		return DC_STATUS_PROTOCOL;
	}

	// Check the type of response.
	// If the response indicates "ok", the payload data is send inline in
	// the remainder of the response packet. If the response indicates "xmr",
	// the payload data is send separately using the XMODEM protocol.
	if (strncmp (buf + cmdlen, ">ok", 3) == 0) {
		// Ignore leading whitespace.
		const char *line = buf + cmdlen + 3;
		while (*line == ' ')
			line++;

		// Copy the payload data.
		size_t len = nbytes - (line - buf);
		if (size) {
			if (len + 1 > size) {
				ERROR (device->base.context, "Unexpected packet length (" DC_PRINTF_SIZE ").", len);
				return DC_STATUS_PROTOCOL;
			}
			memcpy (data, line, len + 1);
		} else {
			if (len != 0) {
				ERROR (device->base.context, "Unexpected packet length (" DC_PRINTF_SIZE ").", len);
				return DC_STATUS_PROTOCOL;
			}
		}
	} else if (strncmp (buf + cmdlen, ">xmr", 4) == 0) {
		if (nbytes > cmdlen + 4) {
			WARNING (device->base.context, "Packet contains extra data ('%s').", buf + cmdlen + 4);
		}
		return oceans_s1_xmodem_recv (device, buffer);
	} else {
		ERROR (device->base.context, "Received unexpected packet data ('%s').", buf);
		return DC_STATUS_PROTOCOL;
	}

	return status;
}

dc_status_t
oceans_s1_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceans_s1_device_t *) dc_device_allocate (context, &oceans_s1_device_vtable);
	if (device == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->timestamp = 0;

	// Set the timeout for receiving data (4000 ms).
	status = dc_iostream_set_timeout (device->iostream, 4000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
oceans_s1_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	oceans_s1_device_t *device = (oceans_s1_device_t *) abstract;

	if (size && size != SZ_FINGERPRINT)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint64_be (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *device = (oceans_s1_device_t *) abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	char version[SZ_PACKET] = {0};
	status = oceans_s1_transfer (device, NULL, version, sizeof(version), "version", NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the version.");
		return status;
	}

	unsigned int major = 0, minor = 0, unknown = 0;
	if (sscanf (version, "%u.%u %x", &major, &minor, &unknown) != 3) {
		ERROR (abstract->context, "Failed to parse the version response.");
		return DC_STATUS_PROTOCOL;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = major << 16 | minor;
	devinfo.serial = 0;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	dc_buffer_t *buffer = dc_buffer_new (4096);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	status = oceans_s1_transfer (device, buffer, NULL, 0, "dllist", NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to download the dive list.");
		goto error_free_buffer;
	}

	const unsigned char *data = dc_buffer_get_data (buffer);
	size_t size = dc_buffer_get_size (buffer);

	oceans_s1_dive_t *logbook = NULL, *dive = NULL;
	unsigned int ndives = 0;

	char *ptr = NULL;
	size_t len = 0;
	int n = 0;
	while ((n = oceans_s1_getline (&ptr, &len, &data, &size)) != -1) {
		// Ignore empty lines.
		if (n == 0)
			continue;

		// Ignore leading whitespace.
		const char *line = ptr;
		while (*line == ' ')
			line++;

		if (strncmp (line, "divelog", 7) == 0 ||
			strncmp (line, "endlog", 6) == 0 ||
			strncmp (line, "continue", 8) == 0) {
			// Nothing to do.
		} else if (strncmp (line, "dive", 4) == 0) {
			if (dive != NULL) {
				ERROR (abstract->context, "Skipping dive without 'enddive' line.");
				free (dive);
				dive = NULL;
			}

			unsigned int number = 0, divemode = 0, o2 = 0;
			dc_ticks_t timestamp = 0;
			if (sscanf (line, "dive %u,%u,%u," DC_FORMAT_INT64, &number, &divemode, &o2, &timestamp) != 4) {
				ERROR (abstract->context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free_list;
			}

			dive = (oceans_s1_dive_t *) malloc (sizeof (oceans_s1_dive_t));
			if (dive == NULL) {
				ERROR (abstract->context, "Failed to allocate memory.");
				status = DC_STATUS_NOMEMORY;
				goto error_free_list;
			}

			dive->next = NULL;
			dive->timestamp = timestamp;
			dive->number = number;
		} else if (strncmp (line, "enddive", 7) == 0) {
			if (dive) {
				if (dive->timestamp > device->timestamp) {
					oceans_s1_list_add (&logbook, dive);
					ndives++;
				} else {
					free (dive);
				}
				dive = NULL;
			} else {
				WARNING (abstract->context, "Unexpected line '%s'.", line);
			}
		} else {
			WARNING (abstract->context, "Unexpected line '%s'.", line);
		}
	}

	if (dive != NULL) {
		WARNING (abstract->context, "Skipping dive without 'enddive' line.");
		free (dive);
		dive = NULL;
	}

	progress.current = 1;
	progress.maximum = 1 + ndives;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	for (dive = logbook; dive; dive = dive->next) {
		status = oceans_s1_transfer (device, buffer, NULL, 0, "dlget", "%u %u", dive->number, dive->number + 1);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the dive.");
			goto error_free_list;
		}

		progress.current++;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		unsigned char fingerprint[SZ_FINGERPRINT] = {0};
		array_uint64_be_set (fingerprint, dive->timestamp);

		if (callback && !callback (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), fingerprint, sizeof(fingerprint), userdata))
			break;
	}

error_free_list:
	oceans_s1_list_free (logbook);
	free (ptr);
error_free_buffer:
	dc_buffer_free (buffer);
error_exit:
	return status;
}

static dc_status_t
oceans_s1_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *device = (oceans_s1_device_t *) abstract;

	// Ignore the timezone offset.
	dc_datetime_t dt = *datetime;
	dt.timezone = DC_TIMEZONE_NONE;

	dc_ticks_t timestamp = dc_datetime_mktime (&dt);
	if (timestamp < 0) {
		ERROR (abstract->context, "Invalid date/time value specified.");
		return DC_STATUS_INVALIDARGS;
	}

	status = oceans_s1_transfer (device, NULL, NULL, 0, "utc", DC_FORMAT_INT64, timestamp);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the date/time.");
		return status;
	}

	return status;
}
