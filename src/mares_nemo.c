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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free

#include <libdivecomputer/mares_nemo.h>

#include "context-private.h"
#include "device-private.h"
#include "mares_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_nemo_device_vtable)

#ifdef PACKETSIZE
#undef PACKETSIZE /* Override the common value. */
#endif

#define MEMORYSIZE 0x4000
#define PACKETSIZE 0x20

#define NEMO        0
#define NEMOEXCEL   17
#define NEMOAPNEIST 18

typedef struct mares_nemo_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned char fingerprint[5];
} mares_nemo_device_t;

static dc_status_t mares_nemo_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_nemo_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_nemo_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mares_nemo_device_close (dc_device_t *abstract);

static const dc_device_vtable_t mares_nemo_device_vtable = {
	sizeof(mares_nemo_device_t),
	DC_FAMILY_MARES_NEMO,
	mares_nemo_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	mares_nemo_device_dump, /* dump */
	mares_nemo_device_foreach, /* foreach */
	mares_nemo_device_close /* close */
};

static const mares_common_layout_t mares_nemo_layout = {
	MEMORYSIZE, /* memsize */
	0x0070, /* rb_profile_begin */
	0x3400, /* rb_profile_end */
	0x3400, /* rb_freedives_begin */
	0x4000  /* rb_freedives_end */
};

static const mares_common_layout_t mares_nemo_apneist_layout = {
	MEMORYSIZE, /* memsize */
	0x0070, /* rb_profile_begin */
	0x0800, /* rb_profile_end */
	0x0800, /* rb_freedives_begin */
	0x4000  /* rb_freedives_end */
};


dc_status_t
mares_nemo_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_nemo_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mares_nemo_device_t *) dc_device_allocate (context, &mares_nemo_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_serial_set_timeout (device->port, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Set the DTR line.
	status = dc_serial_set_dtr (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_close;
	}

	// Set the RTS line.
	status = dc_serial_set_rts (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
mares_nemo_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
mares_nemo_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, MEMORYSIZE)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = MEMORYSIZE + 20;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Wait until some data arrives.
	size_t available = 0;
	while (dc_serial_get_available (device->port, &available) == DC_STATUS_SUCCESS && available == 0) {
		if (device_is_cancelled (abstract))
			return DC_STATUS_CANCELLED;

		device_event_emit (abstract, DC_EVENT_WAITING, NULL);
		dc_serial_sleep (device->port, 100);
	}

	// Receive the header of the package.
	unsigned char header = 0x00;
	for (unsigned int i = 0; i < 20;) {
		status = dc_serial_read (device->port, &header, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the header.");
			return status;
		}
		if (header == 0xEE) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Update and emit a progress event.
	progress.current += 20;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	unsigned int nbytes = 0;
	while (nbytes < MEMORYSIZE) {
		// Read the packet.
		unsigned char packet[(PACKETSIZE + 1) * 2] = {0};
		status = dc_serial_read (device->port, packet, sizeof (packet), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the checksums of the packet.
		unsigned char crc1 = packet[PACKETSIZE];
		unsigned char crc2 = packet[PACKETSIZE * 2 + 1];
		unsigned char ccrc1 = checksum_add_uint8 (packet, PACKETSIZE, 0x00);
		unsigned char ccrc2 = checksum_add_uint8 (packet + PACKETSIZE + 1, PACKETSIZE, 0x00);
		if (crc1 == ccrc1 && crc2 == ccrc2) {
			// Both packets have a correct checksum.
			if (memcmp (packet, packet + PACKETSIZE + 1, PACKETSIZE) != 0) {
				ERROR (abstract->context, "Both packets are not equal.");
				return DC_STATUS_PROTOCOL;
			}
			dc_buffer_append (buffer, packet, PACKETSIZE);
		} else if (crc1 == ccrc1) {
			// Only the first packet has a correct checksum.
			WARNING (abstract->context, "Only the first packet has a correct checksum.");
			dc_buffer_append (buffer, packet, PACKETSIZE);
		} else if (crc2 == ccrc2) {
			// Only the second packet has a correct checksum.
			WARNING (abstract->context, "Only the second packet has a correct checksum.");
			dc_buffer_append (buffer, packet + PACKETSIZE + 1, PACKETSIZE);
		} else {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		progress.current += PACKETSIZE;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += PACKETSIZE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_nemo_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (MEMORYSIZE);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = mares_nemo_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = data[1];
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (data + 8);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = mares_nemo_extract_dives (abstract, data, MEMORYSIZE, callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
mares_nemo_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PACKETSIZE)
		return DC_STATUS_DATAFORMAT;

	dc_context_t *context = (abstract ? abstract->context : NULL);
	unsigned char *fingerprint = (device ? device->fingerprint : NULL);

	const mares_common_layout_t *layout = NULL;
	switch (data[1]) {
	case NEMO:
	case NEMOEXCEL:
		layout = &mares_nemo_layout;
		break;
	case NEMOAPNEIST:
		layout = &mares_nemo_apneist_layout;
		break;
	default: // Unknown, try nemo
		layout = &mares_nemo_layout;
		break;
	}

	if (size < layout->memsize)
		return DC_STATUS_DATAFORMAT;

	return mares_common_extract_dives (context, layout, fingerprint, data, callback, userdata);
}
