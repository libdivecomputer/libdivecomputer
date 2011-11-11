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

#include "device-private.h"
#include "mares_common.h"
#include "mares_nemo.h"
#include "serial.h"
#include "utils.h"
#include "checksum.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#ifdef PACKETSIZE
#undef PACKETSIZE /* Override the common value. */
#endif

#define MEMORYSIZE 0x4000
#define PACKETSIZE 0x20

#define NEMO        0
#define NEMOEXCEL   17
#define NEMOAPNEIST 18

typedef struct mares_nemo_device_t {
	device_t base;
	serial_t *port;
	unsigned char fingerprint[5];
} mares_nemo_device_t;

static device_status_t mares_nemo_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t mares_nemo_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t mares_nemo_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t mares_nemo_device_close (device_t *abstract);

static const device_backend_t mares_nemo_device_backend = {
	DEVICE_TYPE_MARES_NEMO,
	mares_nemo_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
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

static int
device_is_mares_nemo (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_nemo_device_backend;
}


device_status_t
mares_nemo_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_nemo_device_t *device = (mares_nemo_device_t *) malloc (sizeof (mares_nemo_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &mares_nemo_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (9600 8N1).
	rc = serial_configure (device->port, 9600, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR/RTS line.");
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
mares_nemo_device_close (device_t *abstract)
{
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;

	if (! device_is_mares_nemo (abstract))
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


static device_status_t
mares_nemo_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_nemo_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	mares_nemo_device_t *device = (mares_nemo_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_reserve (buffer, MEMORYSIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = MEMORYSIZE + 20;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Wait until some data arrives.
	while (serial_get_received (device->port) == 0) {
		if (device_is_cancelled (abstract))
			return DEVICE_STATUS_CANCELLED;

		device_event_emit (abstract, DEVICE_EVENT_WAITING, NULL);
		serial_sleep (100);
	}

	// Receive the header of the package.
	unsigned char header = 0x00;
	for (unsigned int i = 0; i < 20;) {
		int n = serial_read (device->port, &header, 1);
		if (n != 1) {
			WARNING ("Failed to receive the header.");
			return EXITCODE (n);
		}
		if (header == 0xEE) {
			i++; // Continue.
		} else {
			i = 0; // Reset.
		}
	}

	// Update and emit a progress event.
	progress.current += 20;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	unsigned int nbytes = 0;
	while (nbytes < MEMORYSIZE) {
		// Read the packet.
		unsigned char packet[(PACKETSIZE + 1) * 2] = {0};
		int n = serial_read (device->port, packet, sizeof (packet));
		if (n != sizeof (packet)) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Verify the checksums of the packet.
		unsigned char crc1 = packet[PACKETSIZE];
		unsigned char crc2 = packet[PACKETSIZE * 2 + 1];
		unsigned char ccrc1 = checksum_add_uint8 (packet, PACKETSIZE, 0x00);
		unsigned char ccrc2 = checksum_add_uint8 (packet + PACKETSIZE + 1, PACKETSIZE, 0x00);
		if (crc1 == ccrc1 && crc2 == ccrc2) {
			// Both packets have a correct checksum.
			if (memcmp (packet, packet + PACKETSIZE + 1, PACKETSIZE) != 0) {
				WARNING ("Both packets are not equal.");
				return DEVICE_STATUS_PROTOCOL;
			}
			dc_buffer_append (buffer, packet, PACKETSIZE);
		} else if (crc1 == ccrc1) {
			// Only the first packet has a correct checksum.
			WARNING ("Only the first packet has a correct checksum.");
			dc_buffer_append (buffer, packet, PACKETSIZE);
		} else if (crc2 == ccrc2) {
			// Only the second packet has a correct checksum.
			WARNING ("Only the second packet has a correct checksum.");
			dc_buffer_append (buffer, packet + PACKETSIZE + 1, PACKETSIZE);
		} else {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		// Update and emit a progress event.
		progress.current += PACKETSIZE;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += PACKETSIZE;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_nemo_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (MEMORYSIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = mares_nemo_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = data[1];
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_be (data + 8);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = mares_nemo_extract_dives (abstract, data, MEMORYSIZE, callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
mares_nemo_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	mares_nemo_device_t *device = (mares_nemo_device_t*) abstract;

	if (abstract && !device_is_mares_nemo (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < PACKETSIZE)
		return DEVICE_STATUS_ERROR;

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
		return DEVICE_STATUS_ERROR;

	return mares_common_extract_dives (layout, fingerprint, data, callback, userdata);
}
