/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <libdivecomputer/hw_ostc.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define FW_190 0x015A

#define SZ_MD2HASH 18
#define SZ_EEPROM 256
#define SZ_HEADER 266
#define SZ_FW_190 0x8000
#define SZ_FW_NEW 0x10000

#define WIDTH  320
#define HEIGHT 240
#define BLACK  0x00
#define WHITE  0xFF

typedef struct hw_ostc_device_t {
	dc_device_t base;
	serial_t *port;
	unsigned char fingerprint[5];
} hw_ostc_device_t;

static dc_status_t hw_ostc_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t hw_ostc_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t hw_ostc_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t hw_ostc_device_close (dc_device_t *abstract);

static const device_backend_t hw_ostc_device_backend = {
	DC_FAMILY_HW_OSTC,
	hw_ostc_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	hw_ostc_device_dump, /* dump */
	hw_ostc_device_foreach, /* foreach */
	hw_ostc_device_close /* close */
};


static int
device_is_hw_ostc (dc_device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &hw_ostc_device_backend;
}


static dc_status_t
hw_ostc_send (hw_ostc_device_t *device, unsigned char cmd, unsigned int echo)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command.
	unsigned char command[1] = {cmd};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	if (echo) {
		// Read the echo.
		unsigned char answer[1] = {0};
		n = serial_read (device->port, answer, sizeof (answer));
		if (n != sizeof (answer)) {
			ERROR (abstract->context, "Failed to receive the echo.");
			return EXITCODE (n);
		}

		// Verify the echo.
		if (memcmp (answer, command, sizeof (command)) != 0) {
			ERROR (abstract->context, "Unexpected echo.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	hw_ostc_device_t *device = (hw_ostc_device_t *) malloc (sizeof (hw_ostc_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &hw_ostc_device_backend);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data.
	if (serial_set_timeout (device->port, 4000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_sleep (device->port, 100);
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_device_close (dc_device_t *abstract)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

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
hw_ostc_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_HEADER + SZ_FW_NEW;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'a'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (rc);
	}

	// Read the header.
	unsigned char header[SZ_HEADER] = {0};
	int n = serial_read (device->port, header, sizeof (header));
	if (n != sizeof (header)) {
		ERROR (abstract->context, "Failed to receive the header.");
		return EXITCODE (n);
	}

	// Verify the header.
	unsigned char preamble[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55};
	if (memcmp (header, preamble, sizeof (preamble)) != 0) {
		ERROR (abstract->context, "Unexpected answer header.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the firmware version.
	unsigned int firmware = array_uint16_be (header + 264);

	// Get the amount of profile data.
	unsigned int size = sizeof (header);
	if (firmware > FW_190)
		size += SZ_FW_NEW;
	else
		size += SZ_FW_190;

	// Update and emit a progress event.
	progress.current = sizeof (header);
	progress.maximum = size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, size)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Copy the header to the output buffer.
	memcpy (data, header, sizeof (header));

	unsigned int nbytes = sizeof (header);
	while (nbytes < size) {
		// Set the minimum packet size.
		unsigned int len = 1024;

		// Increase the packet size if more data is immediately available.
		int available = serial_get_received (device->port);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > size)
			len = size - nbytes;

		// Read the packet.
		int n = serial_read (device->port, data + nbytes, len);
		if (n != len) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = hw_ostc_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.firmware = array_uint16_be (data + 264);
	devinfo.serial = array_uint16_le (data + 6);
	if (devinfo.serial > 2048)
		devinfo.model = 2; // OSTC 2N
	else if (devinfo.serial > 300)
		devinfo.model = 1; // OSTC Mk2
	else
		devinfo.model = 0; // OSTC
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	rc = hw_ostc_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
hw_ostc_device_md2hash (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MD2HASH) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	dc_status_t rc = hw_ostc_send (device, 'e', 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the answer.
	int n = serial_read (device->port, data, SZ_MD2HASH);
	if (n != SZ_MD2HASH) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_clock (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	if (datetime == NULL) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	dc_status_t rc = hw_ostc_send (device, 'b', 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the data packet.
	unsigned char packet[6] = {
		datetime->hour, datetime->minute, datetime->second,
		datetime->month, datetime->day, datetime->year - 2000};
	int n = serial_write (device->port, packet, sizeof (packet));
	if (n != sizeof (packet)) {
		ERROR (abstract->context, "Failed to send the data packet.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_eeprom_read (dc_device_t *abstract, unsigned int bank, unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	if (bank > 2) {
		ERROR (abstract->context, "Invalid eeprom bank specified.");
		return DC_STATUS_INVALIDARGS;
	}

	if (size < SZ_EEPROM) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	const unsigned char command[] = {'g', 'j', 'm'};
	dc_status_t rc = hw_ostc_send (device, command[bank], 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the answer.
	int n = serial_read (device->port, data, SZ_EEPROM);
	if (n != SZ_EEPROM) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_eeprom_write (dc_device_t *abstract, unsigned int bank, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	if (bank > 2) {
		ERROR (abstract->context, "Invalid eeprom bank specified.");
		return DC_STATUS_INVALIDARGS;
	}

	if (size != SZ_EEPROM) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	const unsigned char command[] = {'d', 'i', 'n'};
	dc_status_t rc = hw_ostc_send (device, command[bank], 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	for (unsigned int i = 4; i < SZ_EEPROM; ++i) {
		// Send the data byte.
		rc = hw_ostc_send (device, data[i], 1);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_reset (dc_device_t *abstract)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	dc_status_t rc = hw_ostc_send (device, 'h', 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_screenshot (dc_device_t *abstract, dc_buffer_t *buffer, hw_ostc_format_t format)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Bytes per pixel (RGB formats only).
	unsigned int bpp = 0;

	if (format == HW_OSTC_FORMAT_RAW) {
		// The RAW format has a variable size, depending on the actual image
		// content. Usually the total size is around 4K, which is used as an
		// initial guess and expanded when necessary.
		if (!dc_buffer_reserve (buffer, 4096)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}
	} else {
		// The RGB formats have a fixed size, depending only on the dimensions
		// and the number of bytes per pixel. The required amount of memory is
		// allocated immediately.
		bpp = (format == HW_OSTC_FORMAT_RGB16) ? 2 : 3;
		if (!dc_buffer_resize (buffer, WIDTH * HEIGHT * bpp)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = WIDTH * HEIGHT;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command.
	dc_status_t rc = hw_ostc_send (device, 'l', 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Cache the pointer to the image data (RGB formats only).
	unsigned char *image = dc_buffer_get_data (buffer);

	// The OSTC sends the image data in a column by column layout, which is
	// converted on the fly to a more convenient row by row layout as used
	// in the majority of image formats. This conversions requires knowledge
	// of the pixel coordinates.
	unsigned int x = 0, y = 0;

	unsigned int npixels = 0;
	while (npixels < WIDTH * HEIGHT) {
		unsigned char raw[3] = {0};
		int n = serial_read (device->port, raw, 1);
		if (n != 1) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return EXITCODE (n);
		}

		unsigned int nbytes = n;
		unsigned int count = raw[0];
		if ((count & 0x80) == 0x00) {
			// Black pixel.
			raw[1] = raw[2] = BLACK;
			count &= 0x7F;
		} else if ((count & 0xC0) == 0xC0) {
			// White pixel.
			raw[1] = raw[2] = WHITE;
			count &= 0x3F;
		} else {
			// Color pixel.
			n = serial_read (device->port, raw + 1, 2);
			if (n != 2) {
				ERROR (abstract->context, "Failed to receive the packet.");
				return EXITCODE (n);
			}

			nbytes += n;
			count &= 0x3F;
		}
		count++;

		// Check for buffer overflows.
		if (npixels + count > WIDTH * HEIGHT) {
			ERROR (abstract->context, "Unexpected number of pixels received.");
			return DC_STATUS_DATAFORMAT;
		}

		if (format == HW_OSTC_FORMAT_RAW) {
			// Append the raw data to the output buffer.
			dc_buffer_append (buffer, raw, nbytes);
		} else {
			// Store the decompressed data in the output buffer.
			for (unsigned int i = 0; i < count; ++i) {
				// Calculate the offset to the current pixel (row layout)
				unsigned int offset = (y * WIDTH + x) * bpp;

				if (format == HW_OSTC_FORMAT_RGB16) {
					image[offset + 0] = raw[1];
					image[offset + 1] = raw[2];
				} else {
					unsigned int value = (raw[1] << 8) + raw[2];
					unsigned char r = (value & 0xF800) >> 11;
					unsigned char g = (value & 0x07E0) >> 5;
					unsigned char b = (value & 0x001F);
					image[offset + 0] = 255 * r / 31;
					image[offset + 1] = 255 * g / 63;
					image[offset + 2] = 255 * b / 31;
				}

				// Move to the next pixel coordinate (column layout).
				y++;
				if (y == HEIGHT) {
					y = 0;
					x++;
				}
			}
		}

		// Update and emit a progress event.
		progress.current += count;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		npixels += count;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (abstract && !device_is_hw_ostc (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[2] = {0xFA, 0xFA};
	const unsigned char footer[2] = {0xFD, 0xFD};

	// Initialize the data stream pointers.
	const unsigned char *current  = data + size;
	const unsigned char *previous = data + size;

	// Search the data stream for header markers.
	while ((current = array_search_backward (data + 266, current - data - 266, header, sizeof (header))) != NULL) {
		// Move the pointer to the begin of the header.
		current -= sizeof (header);

		// Once a header marker is found, start searching
		// for the corresponding footer marker. The search is
		// now limited to the start of the previous dive.
		previous = array_search_forward (current, previous - current, footer, sizeof (footer));

		if (previous) {
			// Move the pointer to the end of the footer.
			previous += sizeof (footer);

			if (device && memcmp (current + 3, device->fingerprint, sizeof (device->fingerprint)) == 0)
				return DC_STATUS_SUCCESS;

			if (callback && !callback (current, previous - current, current + 3, 5, userdata))
				return DC_STATUS_SUCCESS;
		}

		// Prepare for the next iteration.
		previous = current;
	}

	return DC_STATUS_SUCCESS;
}
