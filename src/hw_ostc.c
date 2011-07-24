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

#include "device-private.h"
#include "hw_ostc.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
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
	device_t base;
	serial_t *port;
	unsigned char fingerprint[5];
} hw_ostc_device_t;

static device_status_t hw_ostc_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t hw_ostc_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t hw_ostc_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t hw_ostc_device_close (device_t *abstract);

static const device_backend_t hw_ostc_device_backend = {
	DEVICE_TYPE_HW_OSTC,
	hw_ostc_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	hw_ostc_device_dump, /* dump */
	hw_ostc_device_foreach, /* foreach */
	hw_ostc_device_close /* close */
};


static int
device_is_hw_ostc (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &hw_ostc_device_backend;
}


static device_status_t
hw_ostc_send (hw_ostc_device_t *device, unsigned char cmd, unsigned int echo)
{
	// Send the command.
	unsigned char command[1] = {cmd};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	if (echo) {
		// Read the echo.
		unsigned char answer[1] = {0};
		n = serial_read (device->port, answer, sizeof (answer));
		if (n != sizeof (answer)) {
			WARNING ("Failed to receive the echo.");
			return EXITCODE (n);
		}

		// Verify the echo.
		if (memcmp (answer, command, sizeof (command)) != 0) {
			WARNING ("Unexpected echo.");
			return DEVICE_STATUS_ERROR;
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	hw_ostc_device_t *device = (hw_ostc_device_t *) malloc (sizeof (hw_ostc_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &hw_ostc_device_backend);

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

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
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
hw_ostc_device_close (device_t *abstract)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
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
hw_ostc_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
hw_ostc_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SZ_HEADER + SZ_FW_NEW;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'a'};
	int rc = serial_write (device->port, command, sizeof (command));
	if (rc != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	// Read the header.
	unsigned char header[SZ_HEADER] = {0};
	int n = serial_read (device->port, header, sizeof (header));
	if (n != sizeof (header)) {
		WARNING ("Failed to receive the header.");
		return EXITCODE (n);
	}

	// Verify the header.
	unsigned char preamble[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55};
	if (memcmp (header, preamble, sizeof (preamble)) != 0) {
		WARNING ("Unexpected answer header.");
		return DEVICE_STATUS_ERROR;
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
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, size)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
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
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
hw_ostc_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = hw_ostc_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = array_uint16_be (data + 264);
	devinfo.serial = array_uint16_le (data + 6);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = hw_ostc_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
hw_ostc_device_md2hash (device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SZ_MD2HASH) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Send the command.
	device_status_t rc = hw_ostc_send (device, 'e', 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Read the answer.
	int n = serial_read (device->port, data, SZ_MD2HASH);
	if (n != SZ_MD2HASH) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_clock (device_t *abstract, const dc_datetime_t *datetime)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (datetime == NULL) {
		WARNING ("Invalid parameter specified.");
		return DEVICE_STATUS_ERROR;
	}

	// Send the command.
	device_status_t rc = hw_ostc_send (device, 'b', 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the data packet.
	unsigned char packet[6] = {
		datetime->hour, datetime->minute, datetime->second,
		datetime->month, datetime->day, datetime->year - 2000};
	int n = serial_write (device->port, packet, sizeof (packet));
	if (n != sizeof (packet)) {
		WARNING ("Failed to send the data packet.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_eeprom_read (device_t *abstract, unsigned int bank, unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (bank > 1) {
		WARNING ("Invalid eeprom bank specified.");
		return DEVICE_STATUS_ERROR;
	}

	if (size < SZ_EEPROM) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Send the command.
	unsigned char command = (bank == 0) ? 'g' : 'j';
	device_status_t rc = hw_ostc_send (device, command, 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Read the answer.
	int n = serial_read (device->port, data, SZ_EEPROM);
	if (n != SZ_EEPROM) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_eeprom_write (device_t *abstract, unsigned int bank, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (bank > 1) {
		WARNING ("Invalid eeprom bank specified.");
		return DEVICE_STATUS_ERROR;
	}

	if (size != SZ_EEPROM) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Send the command.
	unsigned char command = (bank == 0) ? 'd' : 'i';
	device_status_t rc = hw_ostc_send (device, command, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	for (unsigned int i = 4; i < SZ_EEPROM; ++i) {
		// Send the data byte.
		rc = hw_ostc_send (device, data[i], 1);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_reset (device_t *abstract)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command.
	device_status_t rc = hw_ostc_send (device, 'h', 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_device_screenshot (device_t *abstract, dc_buffer_t *buffer, hw_ostc_format_t format)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (! device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Bytes per pixel (RGB formats only).
	unsigned int bpp = 0;

	if (format == HW_OSTC_FORMAT_RAW) {
		// The RAW format has a variable size, depending on the actual image
		// content. Usually the total size is around 4K, which is used as an
		// initial guess and expanded when necessary.
		if (!dc_buffer_reserve (buffer, 4096)) {
			WARNING ("Insufficient buffer space available.");
			return DEVICE_STATUS_MEMORY;
		}
	} else {
		// The RGB formats have a fixed size, depending only on the dimensions
		// and the number of bytes per pixel. The required amount of memory is
		// allocated immediately.
		bpp = (format == HW_OSTC_FORMAT_RGB16) ? 2 : 3;
		if (!dc_buffer_resize (buffer, WIDTH * HEIGHT * bpp)) {
			WARNING ("Insufficient buffer space available.");
			return DEVICE_STATUS_MEMORY;
		}
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = WIDTH * HEIGHT;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the command.
	device_status_t rc = hw_ostc_send (device, 'l', 1);
	if (rc != DEVICE_STATUS_SUCCESS)
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
			WARNING ("Failed to receive the packet.");
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
				WARNING ("Failed to receive the packet.");
				return EXITCODE (n);
			}

			nbytes += n;
			count &= 0x3F;
		}
		count++;

		// Check for buffer overflows.
		if (npixels + count > WIDTH * HEIGHT) {
			WARNING ("Unexpected number of pixels received.");
			return DEVICE_STATUS_ERROR;
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
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		npixels += count;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
hw_ostc_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (abstract && !device_is_hw_ostc (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
				return DEVICE_STATUS_SUCCESS;

			if (callback && !callback (current, previous - current, current + 3, 5, userdata))
				return DEVICE_STATUS_SUCCESS;
		}

		// Prepare for the next iteration.
		previous = current;
	}

	return DEVICE_STATUS_SUCCESS;
}
