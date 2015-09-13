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
#include "ihex.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &hw_ostc_device_vtable)

#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define MAXRETRIES 9

#define FW_190 0x015A

#define SZ_MD2HASH 18
#define SZ_EEPROM 256
#define SZ_HEADER 266
#define SZ_FW_190 0x8000
#define SZ_FW_NEW 0x10000

#define SZ_FIRMWARE 0x17F40
#define SZ_BLOCK    0x40

#define ACK     0x4B /* "K" for ok */
#define NAK     0x4E /* "N" for not ok */
#define PICTYPE 0x57 /* PIC type (18F4685) */

#define WIDTH  320
#define HEIGHT 240
#define BLACK  0x00
#define WHITE  0xFF

typedef struct hw_ostc_device_t {
	dc_device_t base;
	dc_serial_t *port;
	unsigned char fingerprint[5];
} hw_ostc_device_t;

typedef struct hw_ostc_firmware_t {
	unsigned char data[SZ_FIRMWARE];
	unsigned char bitmap[SZ_FIRMWARE / SZ_BLOCK];
} hw_ostc_firmware_t;

static dc_status_t hw_ostc_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t hw_ostc_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t hw_ostc_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t hw_ostc_device_close (dc_device_t *abstract);

static const dc_device_vtable_t hw_ostc_device_vtable = {
	sizeof(hw_ostc_device_t),
	DC_FAMILY_HW_OSTC,
	hw_ostc_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	hw_ostc_device_dump, /* dump */
	hw_ostc_device_foreach, /* foreach */
	hw_ostc_device_close /* close */
};


static dc_status_t
hw_ostc_send (hw_ostc_device_t *device, unsigned char cmd, unsigned int echo)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command.
	unsigned char command[1] = {cmd};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	if (echo) {
		// Read the echo.
		unsigned char answer[1] = {0};
		status = dc_serial_read (device->port, answer, sizeof (answer), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the echo.");
			return status;
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
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (hw_ostc_device_t *) dc_device_allocate (context, &hw_ostc_device_vtable);
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

	// Set the serial communication protocol (115200 8N1).
	status = dc_serial_configure (device->port, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data.
	status = dc_serial_set_timeout (device->port, 4000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Make sure everything is in a sane state.
	dc_serial_sleep (device->port, 100);
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
hw_ostc_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
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
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t*) abstract;

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
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the header.
	unsigned char header[SZ_HEADER] = {0};
	status = dc_serial_read (device->port, header, sizeof (header), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the header.");
		return status;
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
		size_t available = 0;
		status = dc_serial_get_available (device->port, &available);
		if (status == DC_STATUS_SUCCESS && available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > size)
			len = size - nbytes;

		// Read the packet.
		status = dc_serial_read (device->port, data + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
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
	if (devinfo.serial > 7000)
		devinfo.model = 3; // OSTC 2C
	else if (devinfo.serial > 2048)
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
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (!ISINSTANCE (abstract))
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
	status = dc_serial_read (device->port, data, SZ_MD2HASH, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_clock (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (!ISINSTANCE (abstract))
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
	status = dc_serial_write (device->port, packet, sizeof (packet), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the data packet.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_eeprom_read (dc_device_t *abstract, unsigned int bank, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (!ISINSTANCE (abstract))
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
	status = dc_serial_read (device->port, data, SZ_EEPROM, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_device_eeprom_write (dc_device_t *abstract, unsigned int bank, const unsigned char data[], unsigned int size)
{
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (!ISINSTANCE (abstract))
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

	if (!ISINSTANCE (abstract))
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
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;

	if (!ISINSTANCE (abstract))
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
		status = dc_serial_read (device->port, raw, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet.");
			return status;
		}

		unsigned int nbytes = 1;
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
			status = dc_serial_read (device->port, raw + 1, 2, NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the packet.");
				return status;
			}

			nbytes += 2;
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

	if (abstract && !ISINSTANCE (abstract))
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


static dc_status_t
hw_ostc_firmware_readfile (hw_ostc_firmware_t *firmware, dc_context_t *context, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	if (firmware == NULL) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Initialize the buffers.
	memset (firmware->data, 0xFF, sizeof (firmware->data));
	memset (firmware->bitmap, 0x00, sizeof (firmware->bitmap));

	// Open the hex file.
	dc_ihex_file_t *file = NULL;
	rc = dc_ihex_file_open (&file, context, filename);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the hex file.");
		return rc;
	}

	// Read the hex file.
	unsigned int lba = 0;
	dc_ihex_entry_t entry;
	while ((rc = dc_ihex_file_read (file, &entry)) == DC_STATUS_SUCCESS) {
		if (entry.type == 0) {
			// Data record.
			unsigned int address = (lba << 16) + entry.address;
			if (address + entry.length > SZ_FIRMWARE) {
				WARNING (context, "Ignoring out of range record (0x%08x,%u).", address, entry.length);
				continue;
			}

			// Copy the record to the buffer.
			memcpy (firmware->data + address, entry.data, entry.length);

			// Mark the corresponding blocks in the bitmap.
			unsigned int begin = address / SZ_BLOCK;
			unsigned int end = (address + entry.length + SZ_BLOCK - 1) / SZ_BLOCK;
			for (unsigned int i = begin; i < end; ++i) {
				firmware->bitmap[i] = 1;
			}
		} else if (entry.type == 1) {
			// End of file record.
			break;
		} else if (entry.type == 4) {
			// Extended linear address record.
			lba = array_uint16_be (entry.data);
		} else {
			ERROR (context, "Unexpected record type.");
			dc_ihex_file_close (file);
			return DC_STATUS_DATAFORMAT;
		}
	}
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_DONE) {
		ERROR (context, "Failed to read the record.");
		dc_ihex_file_close (file);
		return rc;
	}

	// Close the file.
	dc_ihex_file_close (file);

	// Verify the presence of the first block.
	if (firmware->bitmap[0] == 0) {
		ERROR (context, "No first data block.");
		return DC_STATUS_DATAFORMAT;
	}

	// Setup the last block.
	// Copy the "goto main" instruction, stored in the first 8 bytes of the hex
	// file, to the end of the last block at address 0x17F38. This last block
	// needs to be present, regardless of whether it's included in the hex file
	// or not!
	memset (firmware->data + SZ_FIRMWARE - SZ_BLOCK, 0xFF, SZ_BLOCK - 8);
	memcpy (firmware->data + SZ_FIRMWARE - 8, firmware->data, 8);
	firmware->bitmap[C_ARRAY_SIZE(firmware->bitmap) - 1] = 1;

	// Setup the first block.
	// Copy the hardcoded "goto 0x17F40" instruction to the start of the first
	// block at address 0x00000.
	const unsigned char header[] = {0xA0, 0xEF, 0xBF, 0xF0};
	memcpy (firmware->data, header, sizeof (header));

	return rc;
}


static dc_status_t
hw_ostc_firmware_setup_internal (hw_ostc_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command.
	unsigned char command[1] = {0xC1};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the response.
	unsigned char answer[2] = {0};
	status = dc_serial_read (device->port, answer, sizeof (answer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the response.");
		return status;
	}

	// Verify the response.
	const unsigned char expected[2] = {PICTYPE, ACK};
	if (memcmp (answer, expected, sizeof (expected)) != 0) {
		ERROR (abstract->context, "Unexpected response.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_firmware_setup (hw_ostc_device_t *device, unsigned int maxretries)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int nretries = 0;
	while ((rc = hw_ostc_firmware_setup_internal (device)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			break;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= maxretries)
			break;
	}

	return rc;
}


static dc_status_t
hw_ostc_firmware_write_internal (hw_ostc_device_t *device, unsigned char *data, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the packet.
	status = dc_serial_write (device->port, data, size, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the packet.");
		return status;
	}

	// Read the response.
	unsigned char answer[1] = {0};
	status = dc_serial_read (device->port, answer, sizeof (answer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the response.");
		return status;
	}

	// Verify the response.
	const unsigned char expected[] = {ACK};
	if (memcmp (answer, expected, sizeof (expected)) != 0) {
		ERROR (abstract->context, "Unexpected response.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_firmware_write (hw_ostc_device_t *device, unsigned char *data, unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned int nretries = 0;
	while ((rc = hw_ostc_firmware_write_internal (device, data, size)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			break;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			break;
	}

	return rc;
}

/*
 * Think twice before modifying the code for updating the ostc firmware!
 * It has been carefully developed and tested with assistance from
 * Heinrichs-Weikamp, using a special development unit. If you start
 * experimenting with a normal unit and accidentally screw up, you might
 * brick the device permanently and turn it into an expensive
 * paperweight. You have been warned!
 */
dc_status_t
hw_ostc_device_fwupdate (dc_device_t *abstract, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	hw_ostc_device_t *device = (hw_ostc_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Allocate memory for the firmware data.
	hw_ostc_firmware_t *firmware = (hw_ostc_firmware_t *) malloc (sizeof (hw_ostc_firmware_t));
	if (firmware == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Read the hex file.
	rc = hw_ostc_firmware_readfile (firmware, context, filename);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to read the firmware file.");
		free (firmware);
		return rc;
	}

	// Temporary set a relative short timeout. The command to setup the
	// bootloader needs to be send repeatedly, until the response packet is
	// received. Thus the time between each two attempts is directly controlled
	// by the timeout value.
	dc_serial_set_timeout (device->port, 300);

	// Setup the bootloader.
	const unsigned int baudrates[] = {19200, 115200};
	for (unsigned int i = 0; i < C_ARRAY_SIZE(baudrates); ++i) {
		// Adjust the baudrate.
		rc = dc_serial_configure (device->port, baudrates[i], 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to set the terminal attributes.");
			free (firmware);
			return rc;
		}

		// Try to setup the bootloader.
		unsigned int maxretries = (i == 0 ? 1 : MAXRETRIES);
		rc = hw_ostc_firmware_setup (device, maxretries);
		if (rc == DC_STATUS_SUCCESS)
			break;
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to setup the bootloader.");
		free (firmware);
		return rc;
	}

	// Increase the timeout again.
	dc_serial_set_timeout (device->port, 1000);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = C_ARRAY_SIZE(firmware->bitmap);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	for (unsigned int i = 0; i < C_ARRAY_SIZE(firmware->bitmap); ++i) {
		// Skip empty blocks.
		if (firmware->bitmap[i] == 0)
			continue;

		// Create the packet.
		unsigned int address = i * SZ_BLOCK;
		unsigned char packet[4 + SZ_BLOCK + 1] = {
			(address >> 16) & 0xFF,
			(address >>  8) & 0xFF,
			(address      ) & 0xFF,
			SZ_BLOCK
		};
		memcpy (packet + 4, firmware->data + address, SZ_BLOCK);
		packet[sizeof (packet) - 1] = ~checksum_add_uint8 (packet, 4 + SZ_BLOCK, 0x00) + 1;

		// Send the packet.
		rc = hw_ostc_firmware_write (device, packet, sizeof (packet));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the packet.");
			free (firmware);
			return rc;
		}

		// Update and emit a progress event.
		progress.current = i + 1;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	free (firmware);

	return DC_STATUS_SUCCESS;
}
