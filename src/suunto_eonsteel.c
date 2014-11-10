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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdivecomputer/suunto_eonsteel.h>

#include "context-private.h"
#include "device-private.h"
#include "array.h"

#ifdef HAVE_LIBUSB

#ifdef _WIN32
#define NOGDI
#endif

#include <libusb-1.0/libusb.h>

typedef struct suunto_eonsteel_device_t {
	dc_device_t base;

	libusb_context *ctx;
	libusb_device_handle *handle;
	unsigned int magic;
	unsigned short seq;
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
#define INIT_CMD   0x00
#define INIT_MAGIC 0x0001
#define INIT_SEQ   0

#define READ_STRING_CMD 0x0411

#define FILE_LOOKUP_CMD 0x0010
#define FILE_READ_CMD   0x0110
#define FILE_STAT_CMD   0x0710
#define FILE_CLOSE_CMD  0x0510

#define DIR_LOOKUP_CMD 0x0810
#define READDIR_CMD    0x0910
#define DIR_CLOSE_CMD  0x0a10

static dc_status_t suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_eonsteel_device_close(dc_device_t *abstract);

static const dc_device_vtable_t suunto_eonsteel_device_vtable = {
	DC_FAMILY_SUUNTO_EONSTEEL,
	NULL, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	suunto_eonsteel_device_foreach, /* foreach */
	suunto_eonsteel_device_close /* close */
};

static const char dive_directory[] = "0:/dives";

static struct directory_entry *alloc_dirent(int type, int len, const char *name)
{
	struct directory_entry *res;

	res = malloc(offsetof(struct directory_entry, name) + len + 1);
	if (res) {
		res->next = NULL;
		res->type = type;
		res->namelen = len;
		memcpy(res->name, name, len);
		res->name[len] = 0;
	}
	return res;
}

static void put_le16(unsigned short val, unsigned char *p)
{
	p[0] = val;
	p[1] = val >> 8;
}

static void put_le32(unsigned int val, unsigned char *p)
{
	p[0] = val;
	p[1] = val >> 8;
	p[2] = val >> 16;
	p[3] = val >> 24;
}

static int receive_data(suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	const int InEndpoint = 0x82;
	unsigned char buf[64];
	int ret = 0;

	for (;;) {
		int rc, transferred,  len;

		rc = libusb_interrupt_transfer(eon->handle, InEndpoint, buf, sizeof(buf), &transferred, 5000);
		if (rc || transferred != sizeof(buf)) {
			ERROR(eon->base.context, "incomplete read interrupt transfer");
			return -1;
		}
		// dump every incoming packet?
		HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "rcv", buf, transferred);
		if (buf[0] != 0x3f) {
			ERROR(eon->base.context, "read interrupt transfer returns wrong report type");
			return -1;
		}
		len = buf[1];
		if (len > sizeof(buf)-2) {
			ERROR(eon->base.context, "read interrupt transfer reports short length");
			return -1;
		}
		if (len > size) {
			ERROR(eon->base.context, "read interrupt transfer reports excessive length");
			return -1;
		}
		memcpy(buffer+ret, buf+2, len);
		size -= len;
		ret += len;
		if (len < sizeof(buf)-2)
			break;
	}

	return ret;
}

static int send_cmd(suunto_eonsteel_device_t *eon,
	unsigned short cmd,
	unsigned int len,
	const unsigned char *buffer)
{
	const int OutEndpoint = 0x02;
	unsigned char buf[64];
	int transferred, rc;
	unsigned short seq = eon->seq;
	unsigned int magic = eon->magic;

	// Two-byte packet header, followed by 12 bytes of extended header
	if (len > sizeof(buf)-2-12) {
		ERROR(eon->base.context, "send command with too much long");
		return -1;
	}

	memset(buf, 0, sizeof(buf));

	buf[0] = 0x3f;
	buf[1] = len + 12;

	// 2-byte LE command word
	put_le16(cmd, buf+2);

	// 4-byte LE magic value (starts at 1)
	put_le32(magic, buf+4);

	// 2-byte LE sequence number;
	put_le16(seq, buf+8);

	// 4-byte LE length
	put_le32(len, buf+10);

	// .. followed by actual data
	if (len) {
		memcpy(buf+14, buffer, len);
	}

	rc = libusb_interrupt_transfer(eon->handle, OutEndpoint, buf, sizeof(buf), &transferred, 5000);
	if (rc < 0) {
		ERROR(eon->base.context, "write interrupt transfer failed");
		return -1;
	}

	// dump every outgoing packet?
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "cmd", buf, sizeof(buf));
	return 0;
}

/*
 * Send a command, receive a reply
 *
 * This carefully checks the data fields in the reply for a match
 * against the command, and then only returns the actual reply
 * data itself.
 *
 * Also note that "receive_data()" itself will have removed the
 * per-packet handshake bytes, so unlike "send_cmd()", this does
 * not see the two initial 0x3f 0x?? bytes, and this the offsets
 * for the cmd/magic/seq/len are off by two compared to the
 * send_cmd() side. The offsets are the same in the actual raw
 * packet.
 */
static int send_receive(suunto_eonsteel_device_t *eon,
	unsigned short cmd,
	unsigned int len_out, const unsigned char *out,
	unsigned int len_in, unsigned char *in)
{
	int len, actual;
	unsigned char buf[2048];

	if (send_cmd(eon, cmd, len_out, out) < 0)
		return -1;
	len = receive_data(eon, buf, sizeof(buf));
	if (len < 10) {
		ERROR(eon->base.context, "short command reply (%d)", len);
		return -1;
	}
	if (array_uint16_le(buf) != cmd) {
		ERROR(eon->base.context, "command reply doesn't match command");
		return -1;
	}
	if (array_uint32_le(buf+2) != eon->magic + 5) {
		ERROR(eon->base.context, "command reply doesn't match magic (got %08x, expected %08x)", array_uint32_le(buf+2), eon->magic + 5);
		return -1;
	}
	if (array_uint16_le(buf+6) != eon->seq) {
		ERROR(eon->base.context, "command reply doesn't match sequence number");
		return -1;
	}
	actual = array_uint32_le(buf+8);
	if (actual + 12 != len) {
		ERROR(eon->base.context, "command reply length mismatch (got %d, claimed %d)", len-12, actual);
		return -1;
	}
	if (len_in < actual) {
		ERROR(eon->base.context, "command reply returned too much data (got %d, had %d)", actual, len_in);
		return -1;
	}

	// Successful command - increment sequence number
	eon->seq++;
	memcpy(in, buf+12, actual);
	return actual;
}

static int read_file(suunto_eonsteel_device_t *eon, const char *filename, dc_buffer_t *buf)
{
	unsigned char result[2560];
	unsigned char cmdbuf[64];
	unsigned int size, offset;
	int rc, len;

	memset(cmdbuf, 0, sizeof(cmdbuf));
	len = strlen(filename) + 1;
	if (len + 4 > sizeof(cmdbuf)) {
		ERROR(eon->base.context, "too long filename: %s", filename);
		return -1;
	}
	memcpy(cmdbuf+4, filename, len);
	rc = send_receive(eon, FILE_LOOKUP_CMD,
		len+4, cmdbuf,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "unable to look up %s", filename);
		return -1;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "lookup", result, rc);

	rc = send_receive(eon, FILE_STAT_CMD,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "unable to stat %s", filename);
		return -1;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "stat", result, rc);

	size = array_uint32_le(result+4);
	offset = 0;

	while (size > 0) {
		unsigned int ask, got, at;

		ask = size;
		if (ask > 1024)
			ask = 1024;
		put_le32(1234, cmdbuf+0);	// Not file offset, after all
		put_le32(ask, cmdbuf+4);	// Size of read
		rc = send_receive(eon, FILE_READ_CMD,
			8, cmdbuf,
			sizeof(result), result);
		if (rc < 0) {
			ERROR(eon->base.context, "unable to read %s", filename);
			return -1;
		}
		if (rc < 8) {
			ERROR(eon->base.context, "got short read reply for %s", filename);
			return -1;
		}

		// Not file offset, just stays unmodified.
		at = array_uint32_le(result);
		if (at != 1234) {
			ERROR(eon->base.context, "read of %s returned different offset than asked for (%d vs %d)", filename, at, offset);
			return -1;
		}

		// Number of bytes actually read
		got = array_uint32_le(result+4);
		if (!got)
			break;
		if (rc < 8 + got) {
			ERROR(eon->base.context, "odd read size reply for offset %d of file %s", offset, filename);
			return -1;
		}

		if (got > size)
			got = size;
		dc_buffer_append(buf, result+8, got);
		offset += got;
		size -= got;
	}

	rc = send_receive(eon, FILE_CLOSE_CMD,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "cmd FILE_CLOSE_CMD failed");
		return -1;
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "close", result, rc);

	return offset;
}

/*
 * NOTE! This will create the list of dirent's in reverse order,
 * with the last dirent first. That's intentional: for dives,
 * we will want to look up the last dive first.
 */
static struct directory_entry *parse_dirent(suunto_eonsteel_device_t *eon, int nr, const unsigned char *p, int len, struct directory_entry *old)
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
		entry = alloc_dirent(type, namelen, name);
		entry->next = old;
		old = entry;
	}
	return old;
}

static int get_file_list(suunto_eonsteel_device_t *eon, struct directory_entry **res)
{
	struct directory_entry *de = NULL;
	unsigned char cmd[64];
	unsigned char result[2048];
	int rc, cmdlen;


	*res = NULL;
	put_le32(0, cmd);
	memcpy(cmd + 4, dive_directory, sizeof(dive_directory));
	cmdlen = 4 + sizeof(dive_directory);
	rc = send_receive(eon, DIR_LOOKUP_CMD,
		cmdlen, cmd,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "cmd DIR_LOOKUP failed");
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "DIR_LOOKUP", result, rc);

	for (;;) {
		unsigned int nr, last;

		rc = send_receive(eon, READDIR_CMD,
			0, NULL,
			sizeof(result), result);
		if (rc < 0) {
			ERROR(eon->base.context, "readdir failed");
			return -1;
		}
		if (rc < 8) {
			ERROR(eon->base.context, "short readdir result");
			return -1;
		}
		nr = array_uint32_le(result);
		last = array_uint32_le(result+4);
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "dir packet", result, 8);

		de = parse_dirent(eon, nr, result+8, rc-8, de);
		if (last)
			break;
	}

	rc = send_receive(eon, DIR_CLOSE_CMD,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "dir close failed");
	}

	*res = de;
	return 0;
}

static int initialize_eonsteel(suunto_eonsteel_device_t *eon)
{
	const int InEndpoint = 0x82;
	const unsigned char init[] = {0x02, 0x00, 0x2a, 0x00};
	unsigned char buf[64];

	/* Get rid of any pending stale input first */
	for (;;) {
		int transferred;

		int rc = libusb_interrupt_transfer(eon->handle, InEndpoint, buf, sizeof(buf), &transferred, 10);
		if (rc < 0)
			break;
		if (!transferred)
			break;
	}

	if (send_cmd(eon, INIT_CMD, sizeof(init), init)) {
		ERROR(eon->base.context, "Failed to send initialization command");
		return -1;
	}
	if (receive_data(eon, buf, sizeof(buf)) < 0) {
		ERROR(eon->base.context, "Failed to receive initial reply");
		return -1;
	}

	// Don't ask
	eon->magic = 0x00000005 | (buf[4] << 16) | (buf[5] << 24);
	// Increment the sequence number for every command sent
	eon->seq++;
	return 0;
}

dc_status_t
suunto_eonsteel_device_open(dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	suunto_eonsteel_device_t *eon;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	eon = calloc(1, sizeof(suunto_eonsteel_device_t));
	if (!eon)
		return DC_STATUS_NOMEMORY;

	// Set up the magic handshake fields
	eon->magic = INIT_MAGIC;
	eon->seq = INIT_SEQ;

	// Set up the libdivecomputer interfaces
	device_init(&eon->base, context, &suunto_eonsteel_device_vtable);

	if (libusb_init(&eon->ctx)) {
		ERROR(context, "libusb_init() failed");
		return DC_STATUS_IO;
	}

	eon->handle = libusb_open_device_with_vid_pid(eon->ctx, 0x1493, 0x0030);
	if (!eon->handle) {
		ERROR(context, "unable to open device");
		libusb_exit(eon->ctx);
		return DC_STATUS_IO;
	}

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)
	libusb_set_auto_detach_kernel_driver(eon->handle, 1);
#endif

	libusb_claim_interface(eon->handle, 0);

	if (initialize_eonsteel(eon) < 0) {
		ERROR(context, "unable to initialize device");
		libusb_exit(eon->ctx);
		return DC_STATUS_IO;
	}

	*out = (dc_device_t *) eon;

	return DC_STATUS_SUCCESS;
}

static int count_dir_entries(struct directory_entry *de)
{
	int count = 0;
	while (de) {
		count++;
		de = de->next;
	}
	return count;
}

static dc_status_t
suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	int skip = 0, rc;
	struct directory_entry *de;
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;
	dc_buffer_t *file;
	char pathname[64];
	unsigned int time;
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;

	if (get_file_list(eon, &de) < 0)
		return DC_STATUS_IO;

	file = dc_buffer_new(0);
	progress.maximum = count_dir_entries(de);
	progress.current = 0;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	while (de) {
		int len;
		struct directory_entry *next = de->next;
		unsigned char buf[4];

		if (device_is_cancelled(abstract))
			skip = 1;

		switch (de->type) {
		case DIRTYPE_DIR:
			/* Ignore subdirectories in the dive directory */
			break;
		case DIRTYPE_FILE:
			if (skip)
				break;
			if (sscanf(de->name, "%x.LOG", &time) != 1)
				break;
			len = snprintf(pathname, sizeof(pathname), "%s/%s", dive_directory, de->name);
			if (len >= sizeof(pathname))
				break;

			// Reset the membuffer, put the 4-byte length at the head.
			dc_buffer_clear(file);
			put_le32(time, buf);
			dc_buffer_append(file, buf, 4);

			// Then read the filename into the rest of the buffer
			rc = read_file(eon, pathname, file);
			if (rc < 0)
				break;
			if (!callback)
				break;
			if (!callback(dc_buffer_get_data(file), dc_buffer_get_size(file), NULL, 0, userdata))
				skip = 1;

			// We've used up the buffer, so create a new one
			file = dc_buffer_new(0);
		}
		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

		free(de);
		de = next;
	}
	dc_buffer_free(file);

	return device_is_cancelled(abstract) ? DC_STATUS_CANCELLED : DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_device_close(dc_device_t *abstract)
{
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;

	libusb_close(eon->handle);
	libusb_exit(eon->ctx);
	free(eon);

	return DC_STATUS_SUCCESS;
}

#else // no LIBUSB support

dc_status_t
suunto_eonsteel_device_open(dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	ERROR(context, "The Suunto EON Steel backend needs libusb-1.0");
	return DC_STATUS_UNSUPPORTED;
}

#endif
