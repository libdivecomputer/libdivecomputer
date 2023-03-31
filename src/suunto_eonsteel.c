/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Linus Torvalds
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suunto_eonsteel.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "platform.h"
#include "checksum.h"
#include "hdlc.h"

#define EONSTEEL 0
#define EONCORE  1

typedef struct suunto_eonsteel_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned int model;
	unsigned int magic;
	unsigned short seq;
	unsigned char version[0x30];
	unsigned char fingerprint[4];
} suunto_eonsteel_device_t;

// The EON Steel implements a small filesystem
#define DIRTYPE_FILE 0x0001
#define DIRTYPE_DIR  0x0002

struct directory_entry {
	struct directory_entry *next;
	int type;
	int namelen;
	char name[1];
};

// EON Steel command numbers and other magic field values
#define CMD_INIT	0x0000
#define INIT_MAGIC	0x0001
#define INIT_SEQ	0

#define CMD_READ_STRING	0x0411

#define CMD_FILE_OPEN	0x0010
#define CMD_FILE_READ	0x0110
#define CMD_FILE_STAT	0x0710
#define CMD_FILE_CLOSE	0x0510

#define CMD_DIR_OPEN	0x0810
#define CMD_DIR_READDIR	0x0910
#define CMD_DIR_CLOSE	0x0a10

#define CMD_SET_TIME	0x0003
#define CMD_GET_TIME	0x0103
#define CMD_SET_DATE	0x0203
#define CMD_GET_DATE	0x0303

#define PACKET_SIZE 64
#define HEADER_SIZE 12
#define MAXDATA_SIZE 2048
#define CRC_SIZE    4

static dc_status_t suunto_eonsteel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_eonsteel_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t suunto_eonsteel_device_close (dc_device_t *abstract);

static const dc_device_vtable_t suunto_eonsteel_device_vtable = {
	sizeof(suunto_eonsteel_device_t),
	DC_FAMILY_SUUNTO_EONSTEEL,
	suunto_eonsteel_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	suunto_eonsteel_device_foreach, /* foreach */
	suunto_eonsteel_device_timesync, /* timesync */
	suunto_eonsteel_device_close /* close */
};

static const char dive_directory[] = "0:/dives";

static void file_list_free (struct directory_entry *de)
{
	while (de) {
		struct directory_entry *next = de->next;
		free (de);
		de = next;
	}
}

static struct directory_entry *alloc_dirent(int type, int len, const char *name)
{
	struct directory_entry *res;

	res = (struct directory_entry *) malloc(offsetof(struct directory_entry, name) + len + 1);
	if (res) {
		res->next = NULL;
		res->type = type;
		res->namelen = len;
		memcpy(res->name, name, len);
		res->name[len] = 0;
	}
	return res;
}

/*
 * Get a single 64-byte packet from the dive computer. This handles packet
 * logging and any obvious packet-level errors, and returns the payload of
 * packet.
 *
 * The two first bytes of the packet are packet-level metadata: the report
 * type (always 0x3f), and then the size of the valid data in the packet.
 *
 * The maximum payload is 62 bytes.
 */
static dc_status_t
suunto_eonsteel_receive_usb(suunto_eonsteel_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char buf[PACKET_SIZE];
	size_t transferred = 0;
	unsigned int len = 0;

	rc = dc_iostream_read(device->iostream, buf, sizeof(buf), &transferred);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(device->base.context, "Failed to receive the packet.");
		return rc;
	}

	if (transferred < 2) {
		ERROR(device->base.context, "Invalid packet length (" DC_PRINTF_SIZE ").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	if (buf[0] != 0x3f) {
		ERROR(device->base.context, "Invalid report type (%02x).", buf[0]);
		return DC_STATUS_PROTOCOL;
	}

	len = buf[1];
	if (len + 2 > transferred) {
		ERROR(device->base.context, "Invalid payload length (%u).", len);
		return DC_STATUS_PROTOCOL;
	}
	if (len > size) {
		ERROR(device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_PROTOCOL;
	}

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "rcv", buf + 2, len);

	memcpy(data, buf + 2, len);

	if (actual)
		*actual = len;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_receive_ble(suunto_eonsteel_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char buffer[HEADER_SIZE + MAXDATA_SIZE + CRC_SIZE];
	size_t transferred = 0;

	rc = dc_iostream_read(device->iostream, buffer, sizeof(buffer), &transferred);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(device->base.context, "Failed to receive the packet.");
		return rc;
	}

	if (transferred < CRC_SIZE) {
		ERROR(device->base.context, "Invalid packet length (" DC_PRINTF_SIZE ").", transferred);
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = transferred - CRC_SIZE;

	unsigned int crc = array_uint32_le(buffer + nbytes);
	unsigned int ccrc = checksum_crc32r(buffer, nbytes);
	if (crc != ccrc) {
		ERROR(device->base.context, "Invalid checksum (expected %08x, received %08x).", ccrc, crc);
		return DC_STATUS_PROTOCOL;
	}

	if (nbytes > size) {
		ERROR(device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy(data, buffer, nbytes);

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "rcv", buffer, nbytes);

	if (actual)
		*actual = nbytes;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_send(suunto_eonsteel_device_t *device,
	unsigned short cmd,
	const unsigned char data[],
	unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char buf[PACKET_SIZE + CRC_SIZE];

	// Two-byte packet header, followed by 12 bytes of extended header
	if (size + 2 + HEADER_SIZE + CRC_SIZE > sizeof(buf)) {
		ERROR(device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_PROTOCOL;
	}

	memset(buf, 0, sizeof(buf));

	buf[0] = 0x3f;
	buf[1] = size + HEADER_SIZE;

	// 2-byte LE command word
	array_uint16_le_set(buf + 2, cmd);

	// 4-byte LE magic value (starts at 1)
	array_uint32_le_set(buf + 4, device->magic);

	// 2-byte LE sequence number;
	array_uint16_le_set(buf + 8, device->seq);

	// 4-byte LE length
	array_uint32_le_set(buf + 10, size);

	// .. followed by actual data
	if (size) {
		memcpy(buf + 14, data, size);
	}

	// 4 byte LE checksum
	unsigned int crc = checksum_crc32r(buf + 2, size + HEADER_SIZE);
	array_uint32_le_set(buf + 14 + size, crc);

	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE) {
		rc = dc_iostream_write(device->iostream, buf + 2, size + HEADER_SIZE + CRC_SIZE, NULL);
	} else {
		rc = dc_iostream_write(device->iostream, buf, sizeof(buf) - CRC_SIZE, NULL);
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(device->base.context, "Failed to send the command.");
		return rc;
	}

	HEXDUMP (device->base.context, DC_LOGLEVEL_DEBUG, "cmd", buf + 2, size + HEADER_SIZE);

	return DC_STATUS_SUCCESS;
}

/*
 * Send a command, receive a reply
 *
 * This carefully checks the data fields in the reply for a match
 * against the command, and then only returns the actual reply
 * data itself.
 *
 * Also note that receive() function itself will have removed the
 * per-packet handshake bytes, so unlike the send() function, this
 * functon does not see the two initial 0x3f 0x?? bytes, and thus the
 * offsets for the cmd/magic/seq/len are off by two compared to the
 * send() side. The offsets are the same in the actual raw packet.
 */
static dc_status_t
suunto_eonsteel_transfer(suunto_eonsteel_device_t *device,
	unsigned short cmd,
	const unsigned char data[], unsigned int size,
	unsigned char answer[], unsigned int asize,
	unsigned int *actual)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char header[HEADER_SIZE + MAXDATA_SIZE];
	unsigned int len = 0;

	// Send the command.
	rc = suunto_eonsteel_send(device, cmd, data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE) {
		// Receive the entire data packet.
		rc = suunto_eonsteel_receive_ble(device, header, sizeof(header), &len);
	} else {
		// Receive the header and the first part of the data.
		rc = suunto_eonsteel_receive_usb(device, header, sizeof(header), &len);
	}
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the header length.
	if (len < HEADER_SIZE) {
		ERROR(device->base.context, "Invalid packet length (%u).", len);
		return DC_STATUS_PROTOCOL;
	}

	// Unpack the 12 byte header.
	unsigned int reply = array_uint16_le(header);
	unsigned int magic = array_uint32_le(header + 2);
	unsigned int seq = array_uint16_le(header + 6);
	unsigned int length = array_uint32_le(header + 8);

	if (cmd != CMD_INIT) {
		// Verify the command reply.
		if (reply != cmd) {
			ERROR(device->base.context, "Unexpected command reply (received %04x, expected %04x).", reply, cmd);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the magic value.
		if (magic != device->magic + 5) {
			ERROR(device->base.context, "Unexpected magic value (received %08x, expected %08x).", magic, device->magic + 5);
			return DC_STATUS_PROTOCOL;
		}
	}

	// Verify the sequence number.
	if (seq != device->seq) {
		ERROR(device->base.context, "Unexpected sequence number (received %04x, expected %04x).", seq, device->seq);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the length.
	if (length > asize) {
		ERROR(device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the initial payload length.
	unsigned int nbytes = len - HEADER_SIZE;
	if (nbytes > length) {
		ERROR(device->base.context, "Unexpected number of bytes (received %u, expected %u).", nbytes, length);
		return DC_STATUS_PROTOCOL;
	}

	// Copy the payload data.
	memcpy(answer, header + HEADER_SIZE, nbytes);

	// Receive the remainder of the data.
	if (dc_iostream_get_transport(device->iostream) != DC_TRANSPORT_BLE) {
		while (nbytes < length) {
			rc = suunto_eonsteel_receive_usb(device, answer + nbytes, length - nbytes, &len);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			nbytes += len;

			if (len < PACKET_SIZE - 2)
				break;
		}
	}

	// Verify the total payload length.
	if (nbytes != length) {
		ERROR(device->base.context, "Unexpected number of bytes (received %u, expected %u).", nbytes, length);
		return DC_STATUS_PROTOCOL;
	}

	// Remember the magic number.
	if (cmd == CMD_INIT) {
		device->magic = (magic & 0xffff0000) | 0x0005;
	}

	// Increment the sequence number.
	device->seq++;

	if (actual)
		*actual = nbytes;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
read_file(suunto_eonsteel_device_t *eon, const char *filename, dc_buffer_t *buf)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char result[2560];
	unsigned char cmdbuf[64];
	unsigned int size, offset, len;
	unsigned int n = 0;

	memset(cmdbuf, 0, sizeof(cmdbuf));
	len = strlen(filename) + 1;
	if (len + 4 > sizeof(cmdbuf)) {
		ERROR(eon->base.context, "too long filename: %s", filename);
		return DC_STATUS_PROTOCOL;
	}
	memcpy(cmdbuf+4, filename, len);
	rc = suunto_eonsteel_transfer(eon, CMD_FILE_OPEN,
		cmdbuf, len + 4, result, sizeof(result), &n);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "unable to look up %s", filename);
		return rc;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "lookup", result, n);

	rc = suunto_eonsteel_transfer(eon, CMD_FILE_STAT,
		NULL, 0, result, sizeof(result), &n);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "unable to stat %s", filename);
		return rc;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "stat", result, n);

	size = array_uint32_le(result+4);
	offset = 0;

	while (size > 0) {
		unsigned int ask, got, at;

		ask = size;
		if (ask > 1024)
			ask = 1024;
		array_uint32_le_set(cmdbuf + 0, 1234);	// Not file offset, after all
		array_uint32_le_set(cmdbuf + 4, ask);	// Size of read
		rc = suunto_eonsteel_transfer(eon, CMD_FILE_READ,
			cmdbuf, 8, result, sizeof(result), &n);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR(eon->base.context, "unable to read %s", filename);
			return rc;
		}
		if (n < 8) {
			ERROR(eon->base.context, "got short read reply for %s", filename);
			return DC_STATUS_PROTOCOL;
		}

		// Not file offset, just stays unmodified.
		at = array_uint32_le(result);
		if (at != 1234) {
			ERROR(eon->base.context, "read of %s returned different offset than asked for (%d vs %d)", filename, at, offset);
			return DC_STATUS_PROTOCOL;
		}

		// Number of bytes actually read
		got = array_uint32_le(result+4);
		if (!got)
			break;
		if (n < 8 + got) {
			ERROR(eon->base.context, "odd read size reply for offset %d of file %s", offset, filename);
			return DC_STATUS_PROTOCOL;
		}

		if (got > size)
			got = size;
		if (!dc_buffer_append (buf, result + 8, got)) {
			ERROR (eon->base.context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}
		offset += got;
		size -= got;
	}

	rc = suunto_eonsteel_transfer(eon, CMD_FILE_CLOSE,
		NULL, 0, result, sizeof(result), &n);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "cmd CMD_FILE_CLOSE failed");
		return rc;
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "close", result, n);

	return DC_STATUS_SUCCESS;
}

/*
 * Insert a directory entry in the sorted list, most recent entry
 * first.
 *
 * The directory entry names are the timestamps as hex, so ordering
 * in alphabetical order ends up also ordering in date order!
 */
static struct directory_entry *insert_dirent(struct directory_entry *entry, struct directory_entry *list)
{
	struct directory_entry **pos = &list, *next;

	while ((next = *pos) != NULL) {
		/* Is this bigger (more recent) than the next entry? We're good! */
		if (strcmp(entry->name, next->name) > 0)
			break;
		pos = &next->next;
	}
	entry->next = next;
	*pos = entry;

	return list;
}

/*
 * NOTE! This will create the list of dirent's in reverse order,
 * with the last dirent first. That's intentional: for dives,
 * we will want to look up the last dive first.
 */
static struct directory_entry *parse_dirent(suunto_eonsteel_device_t *eon, int nr, const unsigned char *p, unsigned int len, struct directory_entry *list)
{
	while (len > 8) {
		unsigned int type = array_uint32_le(p);
		unsigned int namelen = array_uint32_le(p+4);
		const unsigned char *name = p+8;
		struct directory_entry *entry;

		if (namelen + 8 + 1 > len || name[namelen] != 0) {
			ERROR(eon->base.context, "corrupt dirent entry");
			break;
		}
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "dir entry", p, 8);

		p += 8 + namelen + 1;
		len -= 8 + namelen + 1;
		entry = alloc_dirent(type, namelen, (const char *) name);
		if (!entry) {
			ERROR(eon->base.context, "out of memory");
			break;
		}
		list = insert_dirent(entry, list);
	}
	return list;
}

static dc_status_t
get_file_list(suunto_eonsteel_device_t *eon, struct directory_entry **res)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	struct directory_entry *de = NULL;
	unsigned char cmd[64];
	unsigned char result[2048];
	unsigned int n = 0;
	unsigned int cmdlen;

	array_uint32_le_set(cmd, 0);
	memcpy(cmd + 4, dive_directory, sizeof(dive_directory));
	cmdlen = 4 + sizeof(dive_directory);
	rc = suunto_eonsteel_transfer(eon, CMD_DIR_OPEN,
		cmd, cmdlen, result, sizeof(result), &n);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "cmd DIR_LOOKUP failed");
		return rc;
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "DIR_LOOKUP", result, n);

	for (;;) {
		unsigned int nr, last;

		rc = suunto_eonsteel_transfer(eon, CMD_DIR_READDIR,
			NULL, 0, result, sizeof(result), &n);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR(eon->base.context, "readdir failed");
			file_list_free(de);
			return rc;
		}
		if (n < 8) {
			ERROR(eon->base.context, "short readdir result");
			file_list_free(de);
			return DC_STATUS_PROTOCOL;
		}
		nr = array_uint32_le(result);
		last = array_uint32_le(result+4);
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "dir packet", result, 8);

		de = parse_dirent(eon, nr, result+8, n-8, de);
		if (last)
			break;
	}

	rc = suunto_eonsteel_transfer(eon, CMD_DIR_CLOSE,
		NULL, 0, result, sizeof(result), NULL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "dir close failed");
		file_list_free(de);
		return rc;
	}

	*res = de;

	return DC_STATUS_SUCCESS;
}

static int
count_file_list(struct directory_entry *list)
{
	int count = 0;

	while (list) {
		count++;
		list = list->next;
	}

	return count;
}

dc_status_t
suunto_eonsteel_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eonsteel_device_t *eon = NULL;
	dc_transport_t transport = dc_iostream_get_transport (iostream);

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	eon = (suunto_eonsteel_device_t *) dc_device_allocate(context, &suunto_eonsteel_device_vtable);
	if (!eon)
		return DC_STATUS_NOMEMORY;

	// Set up the magic handshake fields
	eon->model = model;
	eon->magic = INIT_MAGIC;
	eon->seq = INIT_SEQ;
	memset (eon->version, 0, sizeof (eon->version));
	memset (eon->fingerprint, 0, sizeof (eon->fingerprint));

	if (transport == DC_TRANSPORT_BLE) {
		status = dc_hdlc_open (&eon->iostream, context, iostream, 20, 20);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to create the HDLC stream.");
			goto error_free;
		}
	} else {
		eon->iostream = iostream;
	}

	status = dc_iostream_set_timeout(eon->iostream, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free_iostream;
	}

	const unsigned char init[] = {0x02, 0x00, 0x2a, 0x00};
	status = suunto_eonsteel_transfer(eon, CMD_INIT,
		init, sizeof(init), eon->version, sizeof(eon->version), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "unable to initialize device");
		goto error_free_iostream;
	}

	*out = (dc_device_t *) eon;

	return DC_STATUS_SUCCESS;

error_free_iostream:
	if (transport == DC_TRANSPORT_BLE) {
		dc_iostream_close (eon->iostream);
	}
error_free:
	dc_device_deallocate ((dc_device_t *) eon);
	return status;
}

static dc_status_t
suunto_eonsteel_device_close (dc_device_t *abstract)
{
	suunto_eonsteel_device_t *device = (suunto_eonsteel_device_t *) abstract;

	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE) {
		return dc_iostream_close (device->iostream);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_eonsteel_device_t *device = (suunto_eonsteel_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_status_t rc = DC_STATUS_SUCCESS;
	int skip = 0;
	struct directory_entry *de;
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;
	dc_buffer_t *file;
	char pathname[64];
	unsigned int time;
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = eon->model;
	devinfo.firmware = array_uint32_be (eon->version + 0x20);
	devinfo.serial = array_convert_str2num(eon->version + 0x10, 16);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = get_file_list(eon, &de);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (de == NULL) {
		return DC_STATUS_SUCCESS;
	}

	file = dc_buffer_new (16384);
	if (file == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		file_list_free(de);
		return DC_STATUS_NOMEMORY;
	}

	progress.maximum = count_file_list(de);
	progress.current = 0;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	while (de) {
		int len;
		struct directory_entry *next = de->next;
		unsigned char buf[4];
		const unsigned char *data = NULL;
		unsigned int size = 0;

		if (device_is_cancelled(abstract)) {
			dc_status_set_error(&status, DC_STATUS_CANCELLED);
			skip = 1;
		}

		switch (de->type) {
		case DIRTYPE_DIR:
			/* Ignore subdirectories in the dive directory */
			break;
		case DIRTYPE_FILE:
			if (skip)
				break;

			if (sscanf(de->name, "%x.LOG", &time) != 1) {
				dc_status_set_error(&status, DC_STATUS_PROTOCOL);
				break;
			}

			array_uint32_le_set(buf, time);

			if (memcmp (buf, eon->fingerprint, sizeof (eon->fingerprint)) == 0) {
				skip = 1;
				break;
			}

			len = dc_platform_snprintf(pathname, sizeof(pathname), "%s/%s", dive_directory, de->name);
			if (len < 0 || (unsigned int) len >= sizeof(pathname)) {
				dc_status_set_error(&status, DC_STATUS_PROTOCOL);
				break;
			}

			// Reset the membuffer, put the 4-byte length at the head.
			dc_buffer_clear(file);
			dc_buffer_append(file, buf, 4);

			// Then read the filename into the rest of the buffer
			rc = read_file(eon, pathname, file);
			if (rc != DC_STATUS_SUCCESS) {
				dc_status_set_error(&status, rc);
				break;
			}

			data = dc_buffer_get_data(file);
			size = dc_buffer_get_size(file);

			if (callback && !callback(data, size, data, sizeof(eon->fingerprint), userdata))
				skip = 1;
		}
		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

		free(de);
		de = next;
	}
	dc_buffer_free(file);

	return status;
}

static dc_status_t suunto_eonsteel_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char result[64], cmd[8];
	unsigned int year, month, day;
	unsigned int hour, min, msec;

	year = datetime->year;
	month = datetime->month;
	day = datetime->day;
	hour = datetime->hour;
	min = datetime->minute;
	msec = datetime->second * 1000;

	cmd[0] = year & 0xFF;
	cmd[1] = year >> 8;
	cmd[2] = month;
	cmd[3] = day;
	cmd[4] = hour;
	cmd[5] = min;
	cmd[6] = msec & 0xFF;
	cmd[7] = msec >> 8;

	rc = suunto_eonsteel_transfer(eon, CMD_SET_TIME, cmd, sizeof(cmd), result, sizeof(result), NULL);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	rc = suunto_eonsteel_transfer(eon, CMD_SET_DATE, cmd, sizeof(cmd), result, sizeof(result), NULL);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	return DC_STATUS_SUCCESS;
}
