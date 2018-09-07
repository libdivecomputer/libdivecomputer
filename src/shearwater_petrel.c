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

#include "shearwater_petrel.h"
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
	sizeof(shearwater_petrel_device_t),
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_petrel_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	shearwater_petrel_device_foreach, /* foreach */
	NULL, /* timesync */
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
shearwater_petrel_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	shearwater_petrel_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (shearwater_petrel_device_t *) dc_device_allocate (context, &shearwater_petrel_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Setup the device.
	status = shearwater_common_setup (&device->base, context, iostream);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
shearwater_petrel_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	shearwater_common_device_t *device = (shearwater_common_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Shutdown the device.
	unsigned char request[] = {0x2E, 0x90, 0x20, 0x00};
	rc = shearwater_common_transfer (device, request, sizeof (request), NULL, 0, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
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

	// Enable progress notifications.
	unsigned int current = 0, maximum = 0;
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

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

	// Read the hardware type.
	rc = shearwater_common_identifier (&device->base, buffer, ID_HARDWARE);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the hardware type.");
		dc_buffer_free (buffer);
		dc_buffer_free (manifests);
		return rc;
	}

	// Convert and map to the model number.
	unsigned int hardware = array_uint_be (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer));
	unsigned int model = 0;
	switch (hardware) {
	case 0x0808: // Petrel 2
	case 0x0909: // Petrel 1
	case 0x0B0B: // Petrel 1 (newer hardware)
		model = PETREL;
		break;
	case 0x0A0A: // Nerd 1
		model = NERD;
		break;
	case 0x0E0D: // Nerd 2
		model = NERD2;
		break;
	case 0x0707:
		model = PERDIX;
		break;
	case 0x0C0D:
		model = PERDIXAI;
		break;
	default:
		WARNING (abstract->context, "Unknown hardware type %04x.", hardware);
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model;
	devinfo.firmware = firmware;
	devinfo.serial = array_uint32_be (serial);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	while (1) {
		// Update the progress state.
		// Assume the worst case scenario of a full manifest, and adjust the
		// value with the actual number of dives after the manifest has been
		// processed.
		maximum += 1 + RECORD_COUNT;

		// Download a manifest.
		progress.current = NSTEPS * current;
		progress.maximum = NSTEPS * maximum;
		rc = shearwater_common_download (&device->base, buffer, MANIFEST_ADDR, MANIFEST_SIZE, 0, &progress);
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
		unsigned int count = 0, deleted = 0;
		unsigned int offset = 0;
		while (offset < size) {
			// Check for a valid dive header.
			unsigned int header = array_uint16_be (data + offset);
			if (header == 0x5A23) {
				// this is a deleted dive; keep looking
				offset += RECORD_SIZE;
				deleted++;
				continue;
			}
			if (header != 0xA5C4)
				break;

			// Check the fingerprint data.
			if (memcmp (data + offset + 4, device->fingerprint, sizeof (device->fingerprint)) == 0)
				break;

			offset += RECORD_SIZE;
			count++;
		}

		// Update the progress state.
		current += 1;
		maximum -= RECORD_COUNT - count - deleted;

		// Append the manifest records to the main buffer.
		if (!dc_buffer_append (manifests, data, count * RECORD_SIZE)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			dc_buffer_free (buffer);
			dc_buffer_free (manifests);
			return DC_STATUS_NOMEMORY;
		}

		// Stop downloading manifest if there are no more records.
		if (count + deleted != RECORD_COUNT)
			break;
	}

	// Update and emit a progress event.
	progress.current = NSTEPS * current;
	progress.maximum = NSTEPS * maximum;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Cache the buffer pointer and size.
	unsigned char *data = dc_buffer_get_data (manifests);
	unsigned int size = dc_buffer_get_size (manifests);

	unsigned int offset = 0;
	while (offset < size) {
		// skip deleted dives
		if (array_uint16_be(data + offset) == 0x5A23) {
			offset += RECORD_SIZE;
			continue;
		}
		// Get the address of the dive.
		unsigned int address = array_uint32_be (data + offset + 20);

		// Download the dive.
		progress.current = NSTEPS * current;
		progress.maximum = NSTEPS * maximum;
		rc = shearwater_common_download (&device->base, buffer, DIVE_ADDR + address, DIVE_SIZE, 1, &progress);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to download the dive.");
			dc_buffer_free (buffer);
			dc_buffer_free (manifests);
			return rc;
		}

		// Update the progress state.
		current += 1;

		unsigned char *buf = dc_buffer_get_data (buffer);
		unsigned int len = dc_buffer_get_size (buffer);
		if (callback && !callback (buf, len, buf + 12, sizeof (device->fingerprint), userdata))
			break;

		offset += RECORD_SIZE;
	}

	// Update and emit a progress event.
	progress.current = NSTEPS * current;
	progress.maximum = NSTEPS * maximum;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_free (manifests);
	dc_buffer_free (buffer);

	return rc;
}
