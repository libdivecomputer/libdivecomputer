/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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
#include "atomics_cobalt.h"
#include "checksum.h"
#include "utils.h"
#include "array.h"

#define FP_OFFSET 20

typedef struct atomics_cobalt_device_t {
	device_t base;
	unsigned char fingerprint[6];
} atomics_cobalt_device_t;

static device_status_t atomics_cobalt_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t atomics_cobalt_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t atomics_cobalt_device_close (device_t *abstract);

static const device_backend_t atomics_cobalt_device_backend = {
	DEVICE_TYPE_ATOMICS_COBALT,
	atomics_cobalt_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	atomics_cobalt_device_foreach, /* foreach */
	atomics_cobalt_device_close /* close */
};

static int
device_is_atomics_cobalt (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &atomics_cobalt_device_backend;
}


device_status_t
atomics_cobalt_device_open (device_t **out)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) malloc (sizeof (atomics_cobalt_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &atomics_cobalt_device_backend);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_device_close (device_t *abstract)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
atomics_cobalt_read_dive (device_t *abstract, dc_buffer_t *buffer, int init)
{
	return DEVICE_STATUS_UNSUPPORTED;
}


static device_status_t
atomics_cobalt_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	atomics_cobalt_device_t *device = (atomics_cobalt_device_t *) abstract;

	if (! device_is_atomics_cobalt (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	unsigned int ndives = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = atomics_cobalt_read_dive (abstract, buffer, (ndives == 0))) == DEVICE_STATUS_SUCCESS) {
		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);

		if (size == 0) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (memcmp (data + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		if (callback && !callback (data, size, data + FP_OFFSET, sizeof (device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DEVICE_STATUS_SUCCESS;
		}

		ndives++;
	}

	dc_buffer_free (buffer);

	return rc;
}
