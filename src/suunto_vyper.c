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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>	// assert

#include <libdivecomputer/suunto_vyper.h>

#include "context-private.h"
#include "device-private.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &suunto_vyper_device_vtable)

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define SZ_MEMORY 0x2000
#define SZ_PACKET 32

#define HDR_DEVINFO_VYPER   0x24
#define HDR_DEVINFO_SPYDER  0x16
#define HDR_DEVINFO_BEGIN   (HDR_DEVINFO_SPYDER)
#define HDR_DEVINFO_END     (HDR_DEVINFO_VYPER + 6)

typedef struct suunto_vyper_device_t {
	suunto_common_device_t base;
	dc_serial_t *port;
} suunto_vyper_device_t;

static dc_status_t suunto_vyper_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t suunto_vyper_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t suunto_vyper_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t suunto_vyper_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_vyper_device_close (dc_device_t *abstract);

static const dc_device_vtable_t suunto_vyper_device_vtable = {
	sizeof(suunto_vyper_device_t),
	DC_FAMILY_SUUNTO_VYPER,
	suunto_common_device_set_fingerprint, /* set_fingerprint */
	suunto_vyper_device_read, /* read */
	suunto_vyper_device_write, /* write */
	suunto_vyper_device_dump, /* dump */
	suunto_vyper_device_foreach, /* foreach */
	suunto_vyper_device_close /* close */
};

static const suunto_common_layout_t suunto_vyper_layout = {
	0x51, /* eop */
	0x71, /* rb_profile_begin */
	SZ_MEMORY, /* rb_profile_end */
	9, /* fp_offset */
	5 /* peek */
};

static const suunto_common_layout_t suunto_spyder_layout = {
	0x1C, /* eop */
	0x4C, /* rb_profile_begin */
	SZ_MEMORY, /* rb_profile_end */
	6, /* fp_offset */
	3 /* peek */
};


dc_status_t
suunto_vyper_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_vyper_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (suunto_vyper_device_t *) dc_device_allocate (context, &suunto_vyper_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common_device_init (&device->base);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (2400 8O1).
	status = dc_serial_configure (device->port, 2400, 8, DC_PARITY_ODD, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
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

	// Set the DTR line (power supply for the interface).
	status = dc_serial_set_dtr (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_close;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_serial_sleep (device->port, 100);

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
suunto_vyper_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


static dc_status_t
suunto_vyper_send (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	dc_serial_sleep (device->port, 500);

	// Set RTS to send the command.
	dc_serial_set_rts (device->port, 1);

	// Send the command to the dive computer.
	status = dc_serial_write (device->port, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// If the interface sends an echo back (which is the case for many clone
	// interfaces), this echo should be removed from the input queue before
	// attempting to read the real reply from the dive computer. Otherwise,
	// the data transfer will fail. Timing is also critical here! We have to
	// wait at least until the echo appears (40ms), but not until the reply
	// from the dive computer appears (600ms).
	// The original suunto interface does not have this problem, because it
	// does not send an echo and the RTS switching makes it impossible to
	// receive the reply before RTS is cleared. We have to wait some time
	// before clearing RTS (around 30ms). But if we wait too long (> 500ms),
	// the reply disappears again.
	dc_serial_sleep (device->port, 200);
	dc_serial_purge (device->port, DC_DIRECTION_INPUT);

	// Clear RTS to receive the reply.
	dc_serial_set_rts (device->port, 0);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_transfer (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	assert (asize >= size + 2);

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command to the dive computer.
	dc_status_t rc = suunto_vyper_send (device, command, csize);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	status = dc_serial_read (device->port, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header of the package.
	if (memcmp (command, answer, asize - size - 1) != 0) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SZ_PACKET);

		// Read the package.
		unsigned char answer[SZ_PACKET + 5] = {0};
		unsigned char command[5] = {0x05,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[4] = checksum_xor_uint8 (command, 4, 0x00);
		dc_status_t rc = suunto_vyper_transfer (device, command, sizeof (command), answer, len + 5, len);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 4, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SZ_PACKET);

		// Prepare to write the package.
		unsigned char panswer[3] = {0};
		unsigned char pcommand[3] = {0x07, 0xA5, 0xA2};
		dc_status_t rc = suunto_vyper_transfer (device, pcommand, sizeof (pcommand), panswer, sizeof (panswer), 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char wanswer[5] = {0};
		unsigned char wcommand[SZ_PACKET + 5] = {0x06,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (wcommand + 4, data, len);
		wcommand[len + 4] = checksum_xor_uint8 (wcommand, len + 4, 0x00);
		rc = suunto_vyper_transfer (device, wcommand, len + 5, wanswer, sizeof (wanswer), 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_vyper_read_dive (dc_device_t *abstract, dc_buffer_t *buffer, int init, dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Send the command to the dive computer.
	unsigned char command[3] = {init ? 0x08 : 0x09, 0xA5, 0x00};
	command[2] = checksum_xor_uint8 (command, 2, 0x00);
	dc_status_t rc = suunto_vyper_send (device, command, 3);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return rc;
	}

	unsigned int nbytes = 0;
	for (unsigned int npackages = 0;; ++npackages) {
		// Receive the header of the package.
		size_t n = 0;
		unsigned char answer[SZ_PACKET + 3] = {0};
		status = dc_serial_read (device->port, answer, 2, &n);
		if (status != DC_STATUS_SUCCESS) {
			// If no data is received because a timeout occured, we assume
			// the last package was already received and the transmission
			// can be finished. Unfortunately this is not 100% reliable,
			// because there is always a small chance that more data will
			// arrive later (especially with a short timeout). But it works
			// good enough in practice.
			// Only for the very first package, we can be sure there was
			// an error, because the DC always sends at least one package.
			if (n == 0 && npackages != 0)
				break;
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the header of the package.
		if (answer[0] != command[0] ||
			answer[1] > SZ_PACKET) {
			ERROR (abstract->context, "Unexpected answer start byte(s).");
			return DC_STATUS_PROTOCOL;
		}

		// Receive the remaining part of the package.
		unsigned char len = answer[1];
		status = dc_serial_read (device->port, answer + 2, len + 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the checksum of the package.
		unsigned char crc = answer[len + 2];
		unsigned char ccrc = checksum_xor_uint8 (answer, len + 2, 0x00);
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		// The DC sends a null package (a package with length zero) when it
		// has reached the end of its internal ring buffer. From this point on,
		// the current dive has been overwritten with newer data. Therefore,
		// we discard the current (incomplete) dive and end the transmission.
		if (len == 0) {
			dc_buffer_clear (buffer);
			return DC_STATUS_SUCCESS;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			if (progress->current > progress->maximum)
				progress->current = progress->maximum;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}

		// Append the package to the output buffer.
		// Reporting of buffer errors is delayed until the entire
		// transfer is finished. This approach leaves no data behind in
		// the serial receive buffer, and if this packet is part of the
		// last incomplete dive, no error has to be reported at all.
		dc_buffer_append (buffer, answer + 2, len);

		nbytes += len;

		// If a package is smaller than $SZ_PACKET bytes,
		// we assume it's the last packet and the transmission can be
		// finished early. However, this approach does not work if the
		// last packet is exactly $SZ_PACKET bytes long!
#if 0
		if (len != SZ_PACKET)
			break;
#endif
	}

	// Check for a buffer error.
	if (dc_buffer_get_size (buffer) != nbytes) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// The DC traverses its internal ring buffer backwards. The most recent
	// dive is send first (which allows you to download only the new dives),
	// but also the contents of each dive is reversed. Therefore, we reverse
	// the bytes again before returning them to the application.
	array_reverse_bytes (dc_buffer_get_data (buffer), dc_buffer_get_size (buffer));

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_vyper_device_read_dive (dc_device_t *abstract, dc_buffer_t *buffer, int init)
{
	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	return suunto_vyper_read_dive (abstract, buffer, init, NULL);
}


static dc_status_t
suunto_vyper_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_PACKET);
}


static dc_status_t
suunto_vyper_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t*) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Read the device info. The Vyper and the Spyder store this data
	// in a different location. To minimize the number of (slow) reads,
	// we read a larger block of memory that always contains the data
	// for both devices.
	unsigned char header[HDR_DEVINFO_END - HDR_DEVINFO_BEGIN] = {0};
	dc_status_t rc = suunto_vyper_device_read (abstract, HDR_DEVINFO_BEGIN, header, sizeof (header));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Identify the connected device as a Vyper or a Spyder, by inspecting
	// the Vyper model code. For a Spyder, this value will contain the
	// sample interval (20, 30 or 60s) instead of the model code.
	unsigned int hoffset = HDR_DEVINFO_VYPER - HDR_DEVINFO_BEGIN;
	const suunto_common_layout_t *layout = &suunto_vyper_layout;
	if (header[hoffset] == 20 || header[hoffset] == 30 || header[hoffset] == 60) {
		hoffset = HDR_DEVINFO_SPYDER - HDR_DEVINFO_BEGIN;
		layout = &suunto_spyder_layout;
	}

	// Update and emit a progress event.
	progress.maximum = sizeof (header) +
		(layout->rb_profile_end - layout->rb_profile_begin);
	progress.current += sizeof (header);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = header[hoffset + 0];
	devinfo.firmware = header[hoffset + 1];
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 4; ++i) {
		devinfo.serial *= 100;
		devinfo.serial += header[hoffset + 2 + i];
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (layout->rb_profile_end - layout->rb_profile_begin);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	unsigned int ndives = 0;
	unsigned int remaining = layout->rb_profile_end - layout->rb_profile_begin;
	while ((rc = suunto_vyper_read_dive (abstract, buffer, (ndives == 0), &progress)) == DC_STATUS_SUCCESS) {
		unsigned char *data = dc_buffer_get_data (buffer);
		unsigned int size = dc_buffer_get_size (buffer);

		if (size > remaining) {
			ERROR (abstract->context, "Unexpected number of bytes received.");
			dc_buffer_free (buffer);
			return DC_STATUS_DATAFORMAT;
		}

		if (size == 0) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (memcmp (data + layout->fp_offset, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		if (callback && !callback (data, size, data + layout->fp_offset, sizeof (device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}

		remaining -= size;
		ndives++;
	}

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
suunto_vyper_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t*) abstract;

	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < SZ_MEMORY)
		return DC_STATUS_DATAFORMAT;

	const suunto_common_layout_t *layout = &suunto_vyper_layout;
	if (data[HDR_DEVINFO_VYPER] == 20 || data[HDR_DEVINFO_VYPER] == 30 || data[HDR_DEVINFO_VYPER] == 60)
		layout = &suunto_spyder_layout;

	return suunto_common_extract_dives (device, layout, data, callback, userdata);
}
