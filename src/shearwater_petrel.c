/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include <libdivecomputer/shearwater_petrel.h>

#include "shearwater_common.h"

#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &shearwater_petrel_device_vtable)

#define MANIFEST_ADDR 0xE0000000
#define MANIFEST_SIZE 0x600

#define DIVE_ADDR     0xC0000000
#define DIVE_SIZE     0xFFFFFF

#define RECORD_SIZE   0x20
#define RECORD_COUNT  (MANIFEST_SIZE / RECORD_SIZE)

typedef struct shearwater_petrel_device_t {
	shearwater_common_device_t base;
	unsigned char fingerprint[4];
} shearwater_petrel_device_t;

static dc_status_t shearwater_petrel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t shearwater_petrel_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t shearwater_petrel_device_close (dc_device_t *abstract);

static const dc_device_vtable_t shearwater_petrel_device_vtable = {
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_petrel_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	shearwater_petrel_device_foreach, /* foreach */
	shearwater_petrel_device_close /* close */
};


static unsigned int
str2num (unsigned char data[], unsigned int size, unsigned int offset)
{
	unsigned int value = 0;
	for (unsigned int i = offset; i < size; ++i) {
		if (data[i] < '0' || data[i] > '9')
			break;
		value *= 10;
		value += data[i] - '0';
	}

	return value;
}


dc_status_t
shearwater_petrel_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	shearwater_petrel_device_t *device = (shearwater_petrel_device_t *) malloc (sizeof (shearwater_petrel_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base.base, context, &shearwater_petrel_device_vtable);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	rc = shearwater_common_open (&device->base, context, name);
	if (rc != DC_STATUS_SUCCESS) {
		free (device);
		return rc;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_petrel_device_close (dc_device_t *abstract)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	shearwater_common_device_t *device = (shearwater_common_device_t *) abstract;

	// Shutdown the device.
	unsigned char request[] = {0x2E, 0x90, 0x20, 0x00};
	shearwater_common_transfer (device, request, sizeof (request), NULL, 0, NULL);

	// Close the device.
	rc = shearwater_common_close (device);

	// Free memory.
	free (device);

	return rc;
}


static dc_status_t
shearwater_petrel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	shearwater_petrel_device_t *device = (shearwater_petrel_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_petrel_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	shearwater_petrel_device_t *device = (shearwater_petrel_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Allocate memory buffers for the manifests.
	dc_buffer_t *buffer = dc_buffer_new (MANIFEST_SIZE);
	dc_buffer_t *manifests = dc_buffer_new (MANIFEST_SIZE);
	if (buffer == NULL || manifests == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		dc_buffer_free (buffer);
		dc_buffer_free (manifests);
		return DC_STATUS_NOMEMORY;
	}

	// Read the serial number.
	rc = shearwater_common_identifier (&device->base, buffer, ID_SERIAL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the serial number.");
		dc_buffer_free (buffer);
		dc_buffer_free (manifests);
		return rc;
	}

	// Convert to a number.
	unsigned char serial[4] = {0};
	if (array_convert_hex2bin (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer),
		serial, sizeof (serial)) != 0 ) {
		ERROR (abstract->context, "Failed to convert the serial number.");
		dc_buffer_free (buffer);
		dc_buffer_free (manifests);
		return DC_STATUS_DATAFORMAT;

	}

	// Read the firmware version.
	rc = shearwater_common_identifier (&device->base, buffer, ID_FIRMWARE);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the firmware version.");
		dc_buffer_free (buffer);
		dc_buffer_free (manifests);
		return rc;
	}

	// Convert to a number.
	unsigned int firmware = str2num (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), 1);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 3;
	devinfo.firmware = firmware;
	devinfo.serial = array_uint32_be (serial);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	while (1) {
		// Download a manifest.
		rc = shearwater_common_download (&device->base, buffer, MANIFEST_ADDR, MANIFEST_SIZE, 0);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the manifest.");
			dc_buffer_free (buffer);
			dc_buffer_free (manifests);
			return rc;
		}

		// Cache the buffer pointer and size.
		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);

		// Process the records in the manifest.
		unsigned int count = 0;
		unsigned int offset = 0;
		while (offset < size) {
			// Check for a valid dive header.
			unsigned int header = array_uint16_be (data + offset);
			if (header != 0xA5C4)
				break;

			// Check the fingerprint data.
			if (memcmp (data + offset + 4, device->fingerprint, sizeof (device->fingerprint)) == 0)
				break;

			offset += RECORD_SIZE;
			count++;
		}

		// Append the manifest records to the main buffer.
		if (!dc_buffer_append (manifests, data, count * RECORD_SIZE)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			dc_buffer_free (buffer);
			dc_buffer_free (manifests);
			return DC_STATUS_NOMEMORY;
		}

		// Stop downloading manifest if there are no more records.
		if (count != RECORD_COUNT)
			break;
	}

	// Cache the buffer pointer and size.
	unsigned char *data = dc_buffer_get_data (manifests);
	unsigned int size = dc_buffer_get_size (manifests);

	unsigned int offset = 0;
	while (offset < size) {
		// Get the address of the dive.
		unsigned int address = array_uint32_be (data + offset + 20);

		// Download the dive.
		rc = shearwater_common_download (&device->base, buffer, DIVE_ADDR + address, DIVE_SIZE, 1);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the dive.");
			dc_buffer_free (buffer);
			dc_buffer_free (manifests);
			return rc;
		}

		unsigned char *buf = dc_buffer_get_data (buffer);
		unsigned int len = dc_buffer_get_size (buffer);
		if (callback && !callback (buf, len, buf + 12, sizeof (device->fingerprint), userdata))
			break;

		offset += RECORD_SIZE;
	}

	dc_buffer_free (manifests);
	dc_buffer_free (buffer);

	return rc;
}
