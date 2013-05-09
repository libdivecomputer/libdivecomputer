/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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
#include <assert.h> // assert

#include <libdivecomputer/mares_iconhd.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_iconhd_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#if defined(_WIN32) || defined(__APPLE__)
#define BAUDRATE 256000
#else
#define BAUDRATE 230400
#endif

#define MATRIX    0x0F
#define ICONHD    0x14
#define ICONHDNET 0x15
#define PUCKPRO   0x18
#define NEMOWIDE2 0x19

#define ACK 0xAA
#define EOF 0xEA

typedef struct mares_iconhd_layout_t {
	unsigned int memsize;
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
} mares_iconhd_layout_t;

typedef struct mares_iconhd_device_t {
	dc_device_t base;
	serial_t *port;
	const mares_iconhd_layout_t *layout;
	unsigned char fingerprint[10];
	unsigned char version[140];
	unsigned int packetsize;
} mares_iconhd_device_t;

static dc_status_t mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mares_iconhd_device_close (dc_device_t *abstract);

static const dc_device_vtable_t mares_iconhd_device_vtable = {
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_device_set_fingerprint, /* set_fingerprint */
	mares_iconhd_device_read, /* read */
	NULL, /* write */
	mares_iconhd_device_dump, /* dump */
	mares_iconhd_device_foreach, /* foreach */
	mares_iconhd_device_close /* close */
};

static const mares_iconhd_layout_t mares_iconhd_layout = {
	0x100000, /* memsize */
	0x00A000, /* rb_profile_begin */
	0x100000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_matrix_layout = {
	0x40000, /* memsize */
	0x0A000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
};

static unsigned int
mares_iconhd_get_model (mares_iconhd_device_t *device, unsigned int model)
{
	dc_context_t *context = (device ? ((dc_device_t *) device)->context : NULL);

	// Try to correct an invalid model code using the version packet.
	if (model == 0xFF) {
		WARNING (context, "Invalid model code detected!");
		const unsigned char iconhdnet[] = "Icon AIR";
		if (device && memcmp (device->version + 0x46, iconhdnet, sizeof (iconhdnet) - 1) == 0)
			model = ICONHDNET;
	}

	return model;
}

static dc_status_t
mares_iconhd_transfer (mares_iconhd_device_t *device,
	const unsigned char command[], unsigned int csize,
	unsigned char answer[], unsigned int asize,
	dc_event_progress_t *progress)
{
	dc_device_t *abstract = (dc_device_t *) device;

	assert (csize >= 2);

	// Send the command header to the dive computer.
	int n = serial_write (device->port, command, 2);
	if (n != 2) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the header byte.
	unsigned char header[1] = {0};
	n = serial_read (device->port, header, sizeof (header));
	if (n != sizeof (header)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the header byte.
	if (header[0] != ACK) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Send the command payload to the dive computer.
	if (csize > 2) {
		n = serial_write (device->port, command + 2, csize - 2);
		if (n != csize - 2) {
			ERROR (abstract->context, "Failed to send the command.");
			return EXITCODE (n);
		}
	}

	unsigned int nbytes = 0;
	while (nbytes < asize) {
		// Set the minimum packet size.
		unsigned int len = 1024;

		// Increase the packet size if more data is immediately available.
		int available = serial_get_received (device->port);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > asize)
			len = asize - nbytes;

		// Read the packet.
		n = serial_read (device->port, answer + nbytes, len);
		if (n != len) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		nbytes += len;
	}

	// Receive the trailer byte.
	unsigned char trailer[1] = {0};
	n = serial_read (device->port, trailer, sizeof (trailer));
	if (n != sizeof (trailer)) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the trailer byte.
	if (trailer[0] != EOF) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_version (mares_iconhd_device_t *device, unsigned char data[], unsigned int size)
{
	unsigned char command[] = {0xC2, 0x67};
	return mares_iconhd_transfer (device, command, sizeof (command), data, size, NULL);
}


static dc_status_t
mares_iconhd_read (mares_iconhd_device_t *device, unsigned int address, unsigned char data[], unsigned int size, dc_event_progress_t *progress)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (device->packetsize && len > device->packetsize)
			len = device->packetsize;

		// Read the packet.
		unsigned char command[] = {0xE7, 0x42,
			(address      ) & 0xFF,
			(address >>  8) & 0xFF,
			(address >> 16) & 0xFF,
			(address >> 24) & 0xFF,
			(len      ) & 0xFF,
			(len >>  8) & 0xFF,
			(len >> 16) & 0xFF,
			(len >> 24) & 0xFF};
		rc = mares_iconhd_transfer (device, command, sizeof (command), data, len, progress);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return rc;
}

dc_status_t
mares_iconhd_device_open (dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) malloc (sizeof (mares_iconhd_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &mares_iconhd_device_vtable);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	memset (device->version, 0, sizeof (device->version));
	if (model == NEMOWIDE2 || model == MATRIX || model == PUCKPRO) {
		device->layout = &mares_matrix_layout;
		device->packetsize = 64;
	} else {
		device->layout = &mares_iconhd_layout;
		device->packetsize = 0;
	}

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (256000 8N1).
	if (model == NEMOWIDE2 || model == MATRIX || model == PUCKPRO) {
		rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_EVEN, 1, SERIAL_FLOWCONTROL_NONE);
	} else {
		rc = serial_configure (device->port, BAUDRATE, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	}
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->port, 0) == -1 ||
		serial_set_rts (device->port, 0) == -1)
	{
		ERROR (context, "Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Send the version command.
	dc_status_t status = mares_iconhd_version (device, device->version, sizeof (device->version));
	if (status != DC_STATUS_SUCCESS) {
		serial_close (device->port);
		free (device);
		return status;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_close (dc_device_t *abstract)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t*) abstract;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	return mares_iconhd_read (device, address, data, size, NULL);
}


static dc_status_t
mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = device->layout->memsize;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return mares_iconhd_read (device, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), &progress);
}


static dc_status_t
mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	dc_buffer_t *buffer = dc_buffer_new (device->layout->memsize);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = mares_iconhd_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = mares_iconhd_get_model (device, data[0]);
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (data + 0x0C);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = mares_iconhd_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
mares_iconhd_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const mares_iconhd_layout_t *layout = device->layout;

	if (size < layout->memsize)
		return DC_STATUS_DATAFORMAT;

	// Get the model code.
	unsigned int model = mares_iconhd_get_model (device, data[0]);

	// Get the corresponding dive header size.
	unsigned int header = 0x5C;
	if (model == ICONHDNET)
		header = 0x80;

	// Get the end of the profile ring buffer.
	unsigned int eop = 0;
	const unsigned int config[] = {0x2001, 0x3001};
	for (unsigned int i = 0; i < sizeof (config) / sizeof (*config); ++i) {
		eop = array_uint32_le (data + config[i]);
		if (eop != 0xFFFFFFFF)
			break;
	}
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		ERROR (context, "Ringbuffer pointer out of range.");
		return DC_STATUS_DATAFORMAT;
	}

	// Make the ringbuffer linear, to avoid having to deal with the wrap point.
	unsigned char *buffer = (unsigned char *) malloc (layout->rb_profile_end - layout->rb_profile_begin);
	if (buffer == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	memcpy (buffer + 0, data + eop, layout->rb_profile_end - eop);
	memcpy (buffer + layout->rb_profile_end - eop, data + layout->rb_profile_begin, eop - layout->rb_profile_begin);

	unsigned int offset = layout->rb_profile_end - layout->rb_profile_begin;
	while (offset >= header + 4) {
		// Get the number of samples in the profile data.
		unsigned int nsamples = array_uint16_le (buffer + offset - header + 2);
		if (nsamples == 0xFFFF)
			break;

		// Calculate the total number of bytes for this dive.
		// If the buffer does not contain that much bytes, we reached the
		// end of the ringbuffer. The current dive is incomplete (partially
		// overwritten with newer data), and processing should stop.
		unsigned int nbytes = 4 + header;
		if (model == ICONHDNET)
			nbytes += nsamples * 12 + (nsamples / 4) * 8;
		else
			nbytes += nsamples * 8;
		if (offset < nbytes)
			break;

		// Move to the start of the dive.
		offset -= nbytes;

		// Verify that the length that is stored in the profile data
		// equals the calculated length. If both values are different,
		// something is wrong and an error is returned.
		unsigned int length = array_uint32_le (buffer + offset);
		if (length == 0 || length == 0xFFFFFFFF)
			break;
		if (length != nbytes) {
			ERROR (context, "Calculated and stored size are not equal.");
			free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		unsigned char *fp = buffer + offset + length - header + 6;
		if (device && memcmp (fp, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (buffer + offset, length, fp, sizeof (device->fingerprint), userdata)) {
			free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	free (buffer);

	return DC_STATUS_SUCCESS;
}
