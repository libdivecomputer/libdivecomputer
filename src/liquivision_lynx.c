/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Jef Driesen
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libdivecomputer/units.h>

#include "liquivision_lynx.h"
#include "context-private.h"
#include "device-private.h"
#include "ringbuffer.h"
#include "rbstream.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &liquivision_lynx_device_vtable)

#define XEN  0
#define XEO  1
#define LYNX 2
#define KAON 3

#define XEN_V1   0x83321485 // Not supported
#define XEN_V2   0x83321502
#define XEN_V3   0x83328401

#define XEO_V1_A 0x17485623
#define XEO_V1_B 0x27485623
#define XEO_V2_A 0x17488401
#define XEO_V2_B 0x27488401
#define XEO_V3_A 0x17488402
#define XEO_V3_B 0x27488402

#define LYNX_V1  0x67488403
#define LYNX_V2  0x67488404
#define LYNX_V3  0x67488405

#define KAON_V1  0x37488402
#define KAON_V2  0x47488402

#define MAXRETRIES 2
#define MAXPACKET 12
#define SEGMENTSIZE 0x400
#define PAGESIZE    0x1000
#define MEMSIZE     0x200000

#define RB_LOGBOOK_BEGIN         (1 * PAGESIZE)
#define RB_LOGBOOK_END           (25 * PAGESIZE)
#define RB_LOGBOOK_SIZE          (RB_LOGBOOK_END - RB_LOGBOOK_BEGIN)
#define RB_LOGBOOK_DISTANCE(a,b) ringbuffer_distance (a, b, DC_RINGBUFFER_FULL, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END)

#define RB_PROFILE_BEGIN         (25 * PAGESIZE)
#define RB_PROFILE_END           (500 * PAGESIZE)
#define RB_PROFILE_SIZE          (RB_PROFILE_END - RB_PROFILE_BEGIN)
#define RB_PROFILE_DISTANCE(a,b) ringbuffer_distance (a, b, DC_RINGBUFFER_FULL, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define SZ_HEADER_XEN   80
#define SZ_HEADER_OTHER 96
#define SZ_HEADER_MAX   SZ_HEADER_OTHER

typedef struct liquivision_lynx_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[4];
	unsigned char info[6];
	unsigned char more[12];
} liquivision_lynx_device_t;

static dc_status_t liquivision_lynx_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t liquivision_lynx_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t liquivision_lynx_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t liquivision_lynx_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t liquivision_lynx_device_close (dc_device_t *abstract);

static const dc_device_vtable_t liquivision_lynx_device_vtable = {
	sizeof(liquivision_lynx_device_t),
	DC_FAMILY_LIQUIVISION_LYNX,
	liquivision_lynx_device_set_fingerprint, /* set_fingerprint */
	liquivision_lynx_device_read, /* read */
	NULL, /* write */
	liquivision_lynx_device_dump, /* dump */
	liquivision_lynx_device_foreach, /* foreach */
	NULL, /* timesync */
	liquivision_lynx_device_close /* close */
};

static dc_status_t
liquivision_lynx_send (liquivision_lynx_device_t *device, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Build the packet.
	unsigned char packet[2 + MAXPACKET + 2] = {0};
	packet[0] = 0x00;
	packet[1] = 0xB1;
	if (size) {
		memcpy (packet + 2, data, size);
	}
	packet[2 + size + 0] = 0x0B;
	packet[2 + size + 1] = 0x0E;

	// Send the packet to the device.
	status = dc_iostream_write (device->iostream, packet, size + 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the packet.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_recv (liquivision_lynx_device_t *device, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (size > SEGMENTSIZE)
		return DC_STATUS_INVALIDARGS;

	// Receive the packet from the device.
	unsigned char packet[1 + SEGMENTSIZE + 2] = {0};
	status = dc_iostream_read (device->iostream, packet, 1 + size + 2, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet.");
		return status;
	}

	// Verify the start byte.
	if (packet[0] != 0xC5) {
		ERROR (abstract->context, "Unexpected answer start byte (%02x).", packet[0]);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be (packet + 1 + size);
	unsigned short ccrc = checksum_crc16_ccitt (packet + 1, size, 0xffff, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum (%04x %04x).", crc, ccrc);
		return DC_STATUS_PROTOCOL;
	}

	if (size) {
		memcpy (data, packet + 1, size);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_packet (liquivision_lynx_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	status = liquivision_lynx_send (device, command, csize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	if (asize) {
		status = liquivision_lynx_recv (device, answer, asize);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_transfer (liquivision_lynx_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = liquivision_lynx_packet (device, command, csize, answer, asize)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Delay the next attempt.
		dc_iostream_sleep (device->iostream, 100);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
liquivision_lynx_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	liquivision_lynx_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (liquivision_lynx_device_t *) dc_device_allocate (context, &liquivision_lynx_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (9600 8N1).
	status = dc_iostream_configure (device->iostream, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Set the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	// Set the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Wakeup the device.
	for (unsigned int i = 0; i < 6000; ++i) {
		const unsigned char init[] = {0xAA};
		dc_iostream_write (device->iostream, init, sizeof (init), NULL);
	}

	// Send the info command.
	const unsigned char cmd_info[] = {0x49, 0x4E, 0x46, 0x4F, 0x49, 0x4E, 0x46, 0x4F, 0x49, 0x4E, 0x46, 0x4F};
	status = liquivision_lynx_transfer (device, cmd_info, sizeof(cmd_info), device->info, sizeof(device->info));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the info command.");
		goto error_free;
	}

	// Send the more info command.
	const unsigned char cmd_more[] = {0x4D, 0x4F, 0x52, 0x45, 0x49, 0x4E, 0x46, 0x4F, 0x4D, 0x4F, 0x52, 0x45};
	status = liquivision_lynx_transfer (device, cmd_more, sizeof(cmd_more), device->more, sizeof(device->more));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the more info command.");
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
liquivision_lynx_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	liquivision_lynx_device_t *device = (liquivision_lynx_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	liquivision_lynx_device_t *device = (liquivision_lynx_device_t *) abstract;

	if ((address % SEGMENTSIZE != 0) ||
		(size    % SEGMENTSIZE != 0))
		return DC_STATUS_INVALIDARGS;

	// Get the page and segment number.
	unsigned int page    = (address / PAGESIZE);
	unsigned int segment = (address % PAGESIZE) / SEGMENTSIZE;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		const unsigned char command[] = {
			0x50, 0x41, 0x47, 0x45,
			'0' + ((page / 100) % 10),
			'0' + ((page /  10) % 10),
			'0' + ((page /   1) % 10),
			'0' + ((page / 100) % 10),
			'0' + ((page /  10) % 10),
			'0' + ((page /   1) % 10),
			'0' + segment,
			'0' + segment
		};

		status = liquivision_lynx_transfer (device, command, sizeof(command), data + nbytes, SEGMENTSIZE);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read page %u segment %u.", page, segment);
			return status;
		}

		nbytes += SEGMENTSIZE;
		segment++;
		if (segment == (PAGESIZE / SEGMENTSIZE)) {
			segment = 0;
			page++;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
liquivision_lynx_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	liquivision_lynx_device_t *device = (liquivision_lynx_device_t *) abstract;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_le (device->info + 0);
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (device->more + 0);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, MEMSIZE)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Download the memory dump.
	return device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SEGMENTSIZE);
}

static dc_status_t
liquivision_lynx_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	liquivision_lynx_device_t *device = (liquivision_lynx_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SEGMENTSIZE + RB_LOGBOOK_SIZE + RB_PROFILE_SIZE;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Get the model and version.
	unsigned int model = array_uint16_le (device->info + 0);
	unsigned int version = array_uint32_le (device->info + 2);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model;
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (device->more + 0);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Read the config segment.
	unsigned char config[SEGMENTSIZE] = {0};
	status = liquivision_lynx_device_read (abstract, 0, config, sizeof (config));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory.");
		goto error_exit;
	}

	// Get the header size.
	unsigned int headersize = (model == XEN) ? SZ_HEADER_XEN : SZ_HEADER_OTHER;

	// Get the number of headers per page.
	unsigned int npages = PAGESIZE / headersize;

	// Get the logbook pointers.
	unsigned int begin = array_uint16_le (config + 0x46);
	unsigned int end   = array_uint16_le (config + 0x48);
	unsigned int rb_logbook_begin = RB_LOGBOOK_BEGIN + (begin / npages) * PAGESIZE + (begin % npages) * headersize;
	unsigned int rb_logbook_end   = RB_LOGBOOK_BEGIN + (end   / npages) * PAGESIZE + (end   % npages) * headersize;
	if (rb_logbook_begin < RB_LOGBOOK_BEGIN || rb_logbook_begin > RB_LOGBOOK_END ||
		rb_logbook_end   < RB_LOGBOOK_BEGIN || rb_logbook_end   > RB_LOGBOOK_END) {
		ERROR (abstract->context, "Invalid logbook pointers (%04x, %04x).",
			rb_logbook_begin, rb_logbook_end);
		status = DC_STATUS_DATAFORMAT;
		goto error_exit;
	}

	// Calculate the logbook size.
#if 0
	unsigned int rb_logbook_size = RB_LOGBOOK_DISTANCE (rb_logbook_begin, rb_logbook_end);
#else
	// The logbook begin pointer is explicitly ignored, because it only takes
	// into account dives for which the profile is still available.
	unsigned int rb_logbook_size = RB_LOGBOOK_SIZE;
#endif

	// Get the profile pointers.
	unsigned int rb_profile_begin = array_uint32_le (config + 0x4A);
	unsigned int rb_profile_end   = array_uint32_le (config + 0x4E);
	if (rb_profile_begin < RB_PROFILE_BEGIN || rb_profile_begin > RB_PROFILE_END ||
		rb_profile_end   < RB_PROFILE_BEGIN || rb_profile_end   > RB_PROFILE_END) {
		ERROR (abstract->context, "Invalid profile pointers (%04x, %04x).",
			rb_profile_begin, rb_profile_end);
		status = DC_STATUS_DATAFORMAT;
		goto error_exit;
	}

	// Update and emit a progress event.
	progress.current += SEGMENTSIZE;
	progress.maximum -= RB_LOGBOOK_SIZE - rb_logbook_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the logbook entries.
	unsigned char *logbook = (unsigned char *) malloc (rb_logbook_size);
	if (logbook == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rblogbook = NULL;
	status = dc_rbstream_new (&rblogbook, abstract, SEGMENTSIZE, SEGMENTSIZE, RB_LOGBOOK_BEGIN, RB_LOGBOOK_END, rb_logbook_end, DC_RBSTREAM_BACKWARD);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		goto error_free_logbook;
	}

	// The logbook ringbuffer is read backwards to retrieve the most recent
	// entries first. If an already downloaded entry is identified (by means
	// of its fingerprint), the transfer is aborted immediately to reduce
	// the transfer time.
	unsigned int nbytes = 0;
	unsigned int offset = rb_logbook_size;
	unsigned int address = rb_logbook_end;
	while (nbytes < rb_logbook_size) {
		// Handle the ringbuffer wrap point.
		if (address == RB_LOGBOOK_BEGIN)
			address = RB_LOGBOOK_END;

		// Skip the padding bytes.
		if ((address % PAGESIZE) == 0) {
			unsigned int padding = PAGESIZE % headersize;
			unsigned char dummy[SZ_HEADER_MAX] = {0};
			status = dc_rbstream_read (rblogbook, &progress, dummy, padding);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to read the memory.");
				goto error_free_rblogbook;
			}

			address -= padding;
			nbytes += padding;
		}

		// Move to the start of the current entry.
		address -= headersize;
		offset -= headersize;

		// Read the logbook entry.
		status = dc_rbstream_read (rblogbook, &progress, logbook + offset, headersize);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the memory.");
			goto error_free_rblogbook;
		}

		nbytes += headersize;

		if (array_isequal (logbook + offset, headersize, 0xFF)) {
			offset += headersize;
			break;
		}

		// Verify the checksum.
		unsigned int unused = 2;
		if (version == XEO_V1_A || version == XEO_V1_B) {
			unused = 6;
		}
		unsigned char header[SZ_HEADER_MAX] = {0};
		memcpy (header + 0, device->info + 2, 4);
		memcpy (header + 4, logbook + offset + 4, headersize - 4);
		unsigned int crc  = array_uint32_le (logbook + offset + 0);
		unsigned int ccrc = checksum_crc32 (header, headersize - unused);
		if (crc != ccrc) {
			WARNING (abstract->context, "Invalid dive checksum (%08x %08x)", crc, ccrc);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rblogbook;
		}

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (logbook + offset, device->fingerprint, sizeof(device->fingerprint)) == 0) {
			offset += headersize;
			break;
		}
	}

	// Update and emit a progress event.
	progress.maximum -= rb_logbook_size - nbytes;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Go through the logbook entries a first time, to calculate the total
	// amount of bytes in the profile ringbuffer.
	unsigned int rb_profile_size = 0;

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	unsigned int remaining = RB_PROFILE_SIZE;
	unsigned int previous = rb_profile_end;
	unsigned int entry = rb_logbook_size;
	while (entry != offset) {
		// Move to the start of the current entry.
		entry -= headersize;

		// Get the profile pointer.
		unsigned int current = array_uint32_le (logbook + entry + 16);
		if (current < RB_PROFILE_BEGIN || current >= RB_PROFILE_END) {
			ERROR (abstract->context, "Invalid profile ringbuffer pointer (%08x).", current);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rblogbook;
		}

		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (current, previous);

		// Make sure the profile size is valid.
		if (length > remaining) {
			remaining = 0;
			length = 0;
		}

		// Update the total profile size.
		rb_profile_size += length;

		// Move to the start of the current dive.
		remaining -= length;
		previous = current;
	}

	// At this point, we know the exact amount of data
	// that needs to be transferred for the profiles.
	progress.maximum -= RB_PROFILE_SIZE - rb_profile_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the profile data.
	unsigned char *profile = (unsigned char *) malloc (headersize + rb_profile_size);
	if (profile == NULL) {
		status = DC_STATUS_NOMEMORY;
		goto error_free_rblogbook;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rbprofile = NULL;
	status = dc_rbstream_new (&rbprofile, abstract, SEGMENTSIZE, SEGMENTSIZE, RB_PROFILE_BEGIN, RB_PROFILE_END, rb_profile_end, DC_RBSTREAM_BACKWARD);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		goto error_free_profile;
	}

	// Traverse the logbook ringbuffer backwards to retrieve the most recent
	// dives first. The logbook ringbuffer is linearized at this point, so
	// we do not have to take into account any memory wrapping near the end
	// of the memory buffer.
	remaining = rb_profile_size;
	previous = rb_profile_end;
	entry = rb_logbook_size;
	while (entry != offset) {
		// Move to the start of the current entry.
		entry -= headersize;

		// Get the profile pointer.
		unsigned int current = array_uint32_le (logbook + entry + 16);
		if (current < RB_PROFILE_BEGIN || current >= RB_PROFILE_END) {
			ERROR (abstract->context, "Invalid profile ringbuffer pointer (%08x).", current);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_rbprofile;
		}

		// Calculate the length.
		unsigned int length = RB_PROFILE_DISTANCE (current, previous);

		// Make sure the profile size is valid.
		if (length > remaining) {
			remaining = 0;
			length = 0;
		}

		// Move to the start of the current dive.
		remaining -= length;
		previous = current;

		// Read the dive.
		status = dc_rbstream_read (rbprofile, &progress, profile + remaining + headersize, length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			goto error_free_rbprofile;
		}

		// Prepend the logbook entry to the profile data. The memory buffer is
		// large enough to store this entry. The checksum is replaced with the
		// flash version number.
		memcpy (profile + remaining + 0, device->info + 2, 4);
		memcpy (profile + remaining + 4, logbook + entry + 4, headersize - 4);

		if (callback && !callback (profile + remaining, headersize + length, logbook + entry, sizeof(device->fingerprint), userdata)) {
			break;
		}
	}

error_free_rbprofile:
	dc_rbstream_free (rbprofile);
error_free_profile:
	free (profile);
error_free_rblogbook:
	dc_rbstream_free (rblogbook);
error_free_logbook:
	free (logbook);
error_exit:
	return status;
}

static dc_status_t
liquivision_lynx_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	liquivision_lynx_device_t *device = (liquivision_lynx_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the finish command.
	const unsigned char cmd_finish[] = {0x46, 0x49, 0x4E, 0x49, 0x53, 0x48, 0x46, 0x49, 0x4E, 0x49, 0x53, 0x48};
	status = liquivision_lynx_transfer (device, cmd_finish, sizeof(cmd_finish), NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the finish command.");
		dc_status_set_error(&status, rc);
	}

	return status;
}
