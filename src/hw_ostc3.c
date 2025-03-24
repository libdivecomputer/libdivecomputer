/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
 * Copyright (C) 2014 Anton Lundin
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
#include <stdio.h>  // FILE, fopen

#include "hw_ostc3.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "aes.h"
#include "platform.h"
#include "packet.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &hw_ostc3_device_vtable)

#define OSTC3FW(major,minor) ( \
		(((major) & 0xFF) << 8) | \
		((minor) & 0xFF))

#define SZ_DISPLAY    16
#define SZ_CUSTOMTEXT 60
#define SZ_VERSION    (SZ_CUSTOMTEXT + 4)
#define SZ_HARDWARE   1
#define SZ_HARDWARE2  5
#define SZ_MEMORY     0x400000
#define SZ_CONFIG     4
#define SZ_FWINFO     4
#define SZ_FIRMWARE   0x01E000        // 120KB
#define SZ_FIRMWARE_BLOCK    0x1000   //   4KB
#define SZ_FIRMWARE_BLOCK2   0x0100   //  256B
#define FIRMWARE_AREA      0x3E0000

#define RB_LOGBOOK_SIZE_COMPACT  16
#define RB_LOGBOOK_SIZE_FULL     256
#define RB_LOGBOOK_COUNT 256

#define S_BLOCK_READ 0x20
#define S_BLOCK_WRITE 0x30
#define S_BLOCK_WRITE2 0x31
#define S_ERASE    0x42
#define S_READY    0x4C
#define READY      0x4D
#define S_UPGRADE  0x50
#define HARDWARE2  0x60
#define HEADER     0x61
#define CLOCK      0x62
#define CUSTOMTEXT 0x63
#define DIVE       0x66
#define IDENTITY   0x69
#define HARDWARE   0x6A
#define S_FWINFO   0x6B
#define DISPLAY    0x6E
#define COMPACT    0x6D
#define READ       0x72
#define S_UPLOAD   0x73
#define WRITE      0x77
#define RESET      0x78
#define S_INIT     0xAA
#define INIT       0xBB
#define EXIT       0xFF

#define INVALID    0xFFFFFFFF
#define UNKNOWN    0x00
#define OSTC3      0x0A
#define OSTC4      0x3B
#define SPORT      0x12
#define CR         0x05

#define NODELAY 0
#define TIMEOUT 400

#define HDR_COMPACT_LENGTH   0 // 3 bytes
#define HDR_COMPACT_SUMMARY  3 // 10 bytes
#define HDR_COMPACT_NUMBER  13 // 2 bytes
#define HDR_COMPACT_VERSION 15 // 1 byte

#define HDR_FULL_LENGTH      9 // 3 bytes
#define HDR_FULL_SUMMARY    12 // 10 bytes
#define HDR_FULL_NUMBER     80 // 2 bytes
#define HDR_FULL_VERSION     8 // 1 byte

#define HDR_FULL_POINTERS    2 // 6 bytes
#define HDR_FULL_FIRMWARE   48 // 2 bytes

typedef enum hw_ostc3_state_t {
	OPEN,
	DOWNLOAD,
	SERVICE,
	REBOOTING,
} hw_ostc3_state_t;

typedef struct hw_ostc3_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned int hardware;
	unsigned int feature;
	unsigned int model;
	unsigned int serial;
	unsigned int firmware;
	unsigned char fingerprint[5];
	hw_ostc3_state_t state;
} hw_ostc3_device_t;

typedef struct hw_ostc3_logbook_t {
	unsigned int size;
	unsigned int profile;
	unsigned int fingerprint;
	unsigned int number;
	unsigned int version;
} hw_ostc3_logbook_t;

typedef struct hw_ostc3_firmware_t {
	unsigned char data[SZ_FIRMWARE];
	unsigned int checksum;
} hw_ostc3_firmware_t;

// This key is used both for the OSTC3 and its cousin,
// the OSTC Sport.
// The Frog uses a similar protocol, and with another key.
static const unsigned char ostc3_key[16] = {
	0xF1, 0xE9, 0xB0, 0x30,
	0x45, 0x6F, 0xBE, 0x55,
	0xFF, 0xE7, 0xF8, 0x31,
	0x13, 0x6C, 0xF2, 0xFE
};

static dc_status_t hw_ostc3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t hw_ostc3_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t hw_ostc3_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t hw_ostc3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t hw_ostc3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t hw_ostc3_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t hw_ostc3_device_close (dc_device_t *abstract);

static const dc_device_vtable_t hw_ostc3_device_vtable = {
	sizeof(hw_ostc3_device_t),
	DC_FAMILY_HW_OSTC3,
	hw_ostc3_device_set_fingerprint, /* set_fingerprint */
	hw_ostc3_device_read, /* read */
	hw_ostc3_device_write, /* write */
	hw_ostc3_device_dump, /* dump */
	hw_ostc3_device_foreach, /* foreach */
	hw_ostc3_device_timesync, /* timesync */
	hw_ostc3_device_close /* close */
};

static const hw_ostc3_logbook_t hw_ostc3_logbook_compact = {
	RB_LOGBOOK_SIZE_COMPACT, /* size */
	HDR_COMPACT_LENGTH,      /* profile */
	HDR_COMPACT_SUMMARY,     /* fingerprint */
	HDR_COMPACT_NUMBER,      /* number */
	HDR_COMPACT_VERSION,     /* version */
};

static const hw_ostc3_logbook_t hw_ostc3_logbook_full = {
	RB_LOGBOOK_SIZE_FULL, /* size */
	HDR_FULL_LENGTH,      /* profile */
	HDR_FULL_SUMMARY,     /* fingerprint */
	HDR_FULL_NUMBER,      /* number */
	HDR_FULL_VERSION,     /* version */
};


static int
hw_ostc3_strncpy (unsigned char *data, unsigned int size, const char *text)
{
	// Check the maximum length.
	size_t length = (text ? strlen (text) : 0);
	if (length > size) {
		return -1;
	}

	// Copy the text.
	if (length)
		memcpy (data, text, length);

	// Pad with spaces.
	memset (data + length, 0x20, size - length);

	return 0;
}

static dc_status_t
hw_ostc3_read (hw_ostc3_device_t *device, dc_event_progress_t *progress, unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	size_t nbytes = 0;
	while (nbytes < size) {
		// Set the minimum packet size.
		size_t length = 1024;

		// Limit the packet size to the total size.
		if (nbytes + length > size)
			length = size - nbytes;

		// Read the packet.
		rc = dc_iostream_read (device->iostream, data + nbytes, length, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit ((dc_device_t *) device, DC_EVENT_PROGRESS, progress);
		}

		nbytes += length;
	}

	return rc;
}

static dc_status_t
hw_ostc3_write (hw_ostc3_device_t *device, dc_event_progress_t *progress, const unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	size_t nbytes = 0;
	while (nbytes < size) {
		// Set the maximum packet size.
		size_t length = (device->hardware == OSTC4) ? 64 : 1024;

		// Limit the packet size to the total size.
		if (nbytes + length > size)
			length = size - nbytes;

		// Write the packet.
		rc = dc_iostream_write (device->iostream, data + nbytes, length, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		if (progress) {
			progress->current += length;
			device_event_emit ((dc_device_t *) device, DC_EVENT_PROGRESS, progress);
		}

		nbytes += length;
	}

	return rc;
}

static dc_status_t
hw_ostc3_transfer (hw_ostc3_device_t *device,
                  dc_event_progress_t *progress,
                  unsigned char cmd,
                  const unsigned char input[],
                  unsigned int isize,
                  unsigned char output[],
                  unsigned int osize,
                  unsigned int *actual,
                  unsigned int delay)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned int length = osize;

	if (cmd == DIVE && length < RB_LOGBOOK_SIZE_FULL)
		return DC_STATUS_INVALIDARGS;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Get the correct ready byte for the current state.
	const unsigned char ready = (device->state == SERVICE ? S_READY : READY);

	// Send the command.
	unsigned char command[1] = {cmd};
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the echo.
	unsigned char echo[1] = {0};
	status = dc_iostream_read (device->iostream, echo, sizeof (echo), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the echo.");
		return status;
	}

	// Verify the echo.
	if (memcmp (echo, command, sizeof (command)) != 0) {
		if (echo[0] == ready) {
			ERROR (abstract->context, "Unsupported command.");
			return DC_STATUS_UNSUPPORTED;
		} else {
			ERROR (abstract->context, "Unexpected echo.");
			return DC_STATUS_PROTOCOL;
		}
	}

	if (input) {
		if (cmd == WRITE) {
			// Send the first byte of the input data packet.
			status = hw_ostc3_write (device, progress, input, 1);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to send the data packet.");
				return status;
			}

			dc_iostream_sleep (device->iostream, 10);

			// Send the reamainder of the input data packet.
			status = hw_ostc3_write (device, progress, input + 1, isize - 1);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to send the data packet.");
				return status;
			}
		} else {
			// Send the input data packet.
			status = hw_ostc3_write (device, progress, input, isize);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to send the data packet.");
				return status;
			}
		}
	}

	if (output) {
		if (cmd == DIVE) {
			// Read the dive header.
			status = hw_ostc3_read (device, progress, output, RB_LOGBOOK_SIZE_FULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the dive header.");
				return status;
			}

			// When the hwOS firmware detects the dive profile is no longer
			// valid, it sends a modified dive header (with the begin/end
			// pointer fields reset to zero, and the length field reduced to 8
			// bytes), along with an empty dive profile. Detect this condition
			// and adjust the expected length.
			if (array_isequal (output + HDR_FULL_POINTERS, 6, 0x00) &&
				array_uint24_le (output + HDR_FULL_LENGTH) == 8 &&
				length > RB_LOGBOOK_SIZE_FULL + 5) {
				length = RB_LOGBOOK_SIZE_FULL + 5;
			}

			// Read the dive profile.
			status = hw_ostc3_read (device, progress, output + RB_LOGBOOK_SIZE_FULL, length - RB_LOGBOOK_SIZE_FULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the dive profile.");
				return status;
			}

			// Update and emit a progress event.
			if (progress && osize > length) {
				progress->current += osize - length;
				device_event_emit ((dc_device_t *) device, DC_EVENT_PROGRESS, progress);
			}
		} else {
			// Read the output data packet.
			status = hw_ostc3_read (device, progress, output, length);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the answer.");
				return status;
			}
		}
	}

	if (delay) {
		dc_iostream_poll (device->iostream, delay);
	}

	if (cmd != EXIT) {
		// Read the ready byte.
		unsigned char answer[1] = {0};
		status = dc_iostream_read (device->iostream, answer, sizeof (answer), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the ready byte.");
			return status;
		}

		// Verify the ready byte.
		if (answer[0] != ready) {
			ERROR (abstract->context, "Unexpected ready byte.");
			return DC_STATUS_PROTOCOL;
		}
	}

	if (actual)
		*actual = length;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = NULL;
	dc_transport_t transport = dc_iostream_get_transport (iostream);

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (hw_ostc3_device_t *) dc_device_allocate (context, &hw_ostc3_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->hardware = INVALID;
	device->feature = 0;
	device->model = 0;
	device->serial = 0;
	device->firmware = 0;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Create the packet stream.
	if (transport == DC_TRANSPORT_BLE) {
		status = dc_packet_open (&device->iostream, context, iostream, 244, 20);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to create the packet stream.");
			goto error_free;
		}
	} else {
		device->iostream = iostream;
	}

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free_iostream;
	}

	// Set the timeout for receiving data (3000ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free_iostream;
	}

	// Make sure everything is in a sane state.
	dc_iostream_sleep (device->iostream, 300);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	device->state = OPEN;

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free_iostream:
	if (transport == DC_TRANSPORT_BLE) {
		dc_iostream_close (device->iostream);
	}
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
hw_ostc3_device_id (hw_ostc3_device_t *device, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (size != SZ_HARDWARE && size != SZ_HARDWARE2)
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char hardware[SZ_HARDWARE2] = {0};
	status = hw_ostc3_transfer (device, NULL, HARDWARE2, NULL, 0, hardware, SZ_HARDWARE2, NULL, NODELAY);
	if (status == DC_STATUS_UNSUPPORTED) {
		status = hw_ostc3_transfer (device, NULL, HARDWARE, NULL, 0, hardware + 1, SZ_HARDWARE, NULL, NODELAY);
	}
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (size == SZ_HARDWARE2) {
		memcpy (data, hardware, SZ_HARDWARE2);
	} else {
		memcpy (data, hardware + 1, SZ_HARDWARE);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_init_download (hw_ostc3_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Send the init command.
	dc_status_t status = hw_ostc3_transfer (device, NULL, INIT, NULL, 0, NULL, 0, NULL, NODELAY);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send the command.");
		return status;
	}

	device->state = DOWNLOAD;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_init_service (hw_ostc3_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	const unsigned char command[] = {S_INIT, 0xAB, 0xCD, 0xEF};
	unsigned char answer[5] = {0};

	// Send the command and service key.
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Read the response.
	status = dc_iostream_read (device->iostream, answer, sizeof (answer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the response to service mode.
	if (answer[0] != 0x4B || answer[1] != 0xAB ||
			answer[2] != 0xCD || answer[3] != 0xEF ||
			answer[4] != S_READY) {
		ERROR (abstract->context, "Failed to verify the answer.");
		return DC_STATUS_PROTOCOL;
	}

	device->state = SERVICE;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_init (hw_ostc3_device_t *device, hw_ostc3_state_t state)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device->state == state) {
		// No change.
		rc = DC_STATUS_SUCCESS;
	} else if (device->state == OPEN) {
		// Change to download or service mode.
		if (state == DOWNLOAD) {
			rc = hw_ostc3_device_init_download(device);
		} else if (state == SERVICE) {
			rc = hw_ostc3_device_init_service(device);
		} else {
			rc = DC_STATUS_INVALIDARGS;
		}
	} else if (device->state == SERVICE && state == DOWNLOAD) {
		// Switching between service and download mode is not possible.
		// But in service mode, all download commands are supported too,
		// so there is no need to change the state.
		rc = DC_STATUS_SUCCESS;
	} else {
		// Not supported.
		rc = DC_STATUS_INVALIDARGS;
	}

	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (device->hardware != INVALID)
		return DC_STATUS_SUCCESS;

	// Read the hardware descriptor.
	unsigned char hardware[SZ_HARDWARE2] = {0, UNKNOWN};
	rc = hw_ostc3_device_id (device, hardware, sizeof(hardware));
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR (abstract->context, "Failed to read the hardware descriptor.");
		return rc;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Hardware", hardware, sizeof(hardware));

	// Read the version information.
	unsigned char version[SZ_VERSION] = {0};
	rc = hw_ostc3_transfer (device, NULL, IDENTITY, NULL, 0, version, sizeof(version), NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the version information.");
		return rc;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Version", version, sizeof(version));

	// Cache the descriptor.
	device->hardware = array_uint16_be(hardware + 0);
	device->feature = array_uint16_be(hardware + 2);
	device->model = hardware[4];
	device->serial = array_uint16_le (version + 0);
	if (device->hardware == OSTC4) {
		device->firmware = array_uint16_le (version + 2);
	} else {
		device->firmware = array_uint16_be (version + 2);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the exit command
	if (device->state == DOWNLOAD || device->state == SERVICE) {
		rc = hw_ostc3_transfer (device, NULL, EXIT, NULL, 0, NULL, 0, NULL, NODELAY);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			dc_status_set_error(&status, rc);
		}
	}

	// Close the packet stream.
	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE) {
		rc = dc_iostream_close (device->iostream);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to close the packet stream.");
			dc_status_set_error(&status, rc);
		}
	}

	return status;
}


static dc_status_t
hw_ostc3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size != SZ_VERSION)
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, IDENTITY, NULL, 0, data, size, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_hardware (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size != SZ_HARDWARE && size != SZ_HARDWARE2)
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_device_id (device, data, size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.firmware = device->firmware;
	devinfo.serial = device->serial;
	if (device->hardware != UNKNOWN) {
		devinfo.model = device->hardware;
	} else {
		// Fallback to the serial number.
		if (devinfo.serial > 10000)
			devinfo.model = SPORT;
		else
			devinfo.model = OSTC3;
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate memory.
	unsigned char *header = (unsigned char *) malloc (RB_LOGBOOK_SIZE_FULL * RB_LOGBOOK_COUNT);
	if (header == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Download the compact logbook headers. If the firmware doesn't support
	// compact headers yet, fallback to downloading the full logbook headers.
	// This is slower, but also works for older firmware versions.
	unsigned int compact = 1;
	rc = hw_ostc3_transfer (device, &progress, COMPACT,
              NULL, 0, header, RB_LOGBOOK_SIZE_COMPACT * RB_LOGBOOK_COUNT, NULL, NODELAY);
	if (rc == DC_STATUS_UNSUPPORTED) {
		compact = 0;
		rc = hw_ostc3_transfer (device, &progress, HEADER,
		          NULL, 0, header, RB_LOGBOOK_SIZE_FULL * RB_LOGBOOK_COUNT, NULL, NODELAY);
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the header.");
		free (header);
		return rc;
	}

	// Get the correct logbook layout.
	const hw_ostc3_logbook_t *logbook = NULL;
	if (compact) {
		logbook = &hw_ostc3_logbook_compact;
	} else {
		logbook = &hw_ostc3_logbook_full;
	}

	// Locate the most recent dive.
	// The device maintains an internal counter which is incremented for every
	// dive, and the current value at the time of the dive is stored in the
	// dive header. Thus the most recent dive will have the highest value.
	unsigned int latest = 0;
	unsigned int maximum = 0;
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		unsigned int offset = i * logbook->size;

		// Ignore uninitialized header entries.
		if (array_isequal (header + offset, logbook->size, 0xFF))
			continue;

		// Get the internal dive number.
		unsigned int current = array_uint16_le (header + offset + logbook->number);
		if (current > maximum || device->hardware == OSTC4) {
			maximum = current;
			latest = i;
		}
	}

	// Calculate the total and maximum size.
	unsigned int ndives = 0;
	unsigned int size = 0;
	unsigned int maxsize = 0;
	unsigned char dive[RB_LOGBOOK_COUNT] = {0};
	for (unsigned int i = 0; i < RB_LOGBOOK_COUNT; ++i) {
		unsigned int idx = (latest + RB_LOGBOOK_COUNT - i) % RB_LOGBOOK_COUNT;
		unsigned int offset = idx * logbook->size;

		// Ignore uninitialized header entries.
		if (array_isequal (header + offset, logbook->size, 0xFF)) {
			WARNING (abstract->context, "Unexpected empty header found.");
			continue;
		}

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE_FULL + array_uint24_le (header + offset + logbook->profile) - 3;
		if (!compact) {
			// Workaround for a bug in older firmware versions.
			unsigned int firmware = array_uint16_be (header + offset + HDR_FULL_FIRMWARE);
			if (firmware < OSTC3FW(0,93))
				length -= 3;
		}
		if (length < RB_LOGBOOK_SIZE_FULL) {
			ERROR (abstract->context, "Invalid profile length (%u bytes).", length);
			free (header);
			return DC_STATUS_DATAFORMAT;
		}

		// Check the fingerprint data.
		if (memcmp (header + offset + logbook->fingerprint, device->fingerprint, sizeof (device->fingerprint)) == 0)
			break;

		if (length > maxsize)
			maxsize = length;
		size += length;
		dive[ndives] = idx;
		ndives++;
	}

	// Update and emit a progress event.
	progress.maximum = (logbook->size * RB_LOGBOOK_COUNT) + size + ndives;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Finish immediately if there are no dives available.
	if (ndives == 0) {
		free (header);
		return DC_STATUS_SUCCESS;
	}

	// Allocate enough memory for the largest dive.
	unsigned char *profile = (unsigned char *) malloc (maxsize);
	if (profile == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		free (header);
		return DC_STATUS_NOMEMORY;
	}

	// Download the dives.
	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int idx = dive[i];
		unsigned int offset = idx * logbook->size;

		// Calculate the profile length.
		unsigned int length = RB_LOGBOOK_SIZE_FULL + array_uint24_le (header + offset + logbook->profile) - 3;
		if (!compact) {
			// Workaround for a bug in older firmware versions.
			unsigned int firmware = array_uint16_be (header + offset + HDR_FULL_FIRMWARE);
			if (firmware < OSTC3FW(0,93))
				length -= 3;
		}

		// Download the dive.
		unsigned char number[1] = {idx};
		rc = hw_ostc3_transfer (device, &progress, DIVE,
			number, sizeof (number), profile, length, &length, NODELAY);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			free (profile);
			free (header);
			return rc;
		}

		// Verify the header in the logbook and profile are identical.
		if (memcmp (profile + HDR_FULL_VERSION, header + offset + logbook->version, 1) != 0 ||
			compact ?
			memcmp (profile + HDR_FULL_SUMMARY, header + offset + HDR_COMPACT_SUMMARY, 10) != 0 ||
			memcmp (profile + HDR_FULL_NUMBER, header + offset + HDR_COMPACT_NUMBER, 2) != 0 :
			memcmp (profile + HDR_FULL_SUMMARY, header + offset + HDR_FULL_SUMMARY, RB_LOGBOOK_SIZE_FULL - HDR_FULL_SUMMARY) != 0) {
			ERROR (abstract->context, "Unexpected profile header.");
			free (profile);
			free (header);
			return DC_STATUS_DATAFORMAT;
		}

		// Detect invalid profile data.
		unsigned int delta = device->hardware == OSTC4 ? 3 : 0;
		if (length < RB_LOGBOOK_SIZE_FULL + 2 ||
			profile[length - 2] != 0xFD || profile[length - 1] != 0xFD) {
			// A valid profile should have at least a correct 2 byte
			// end-of-profile marker.
			WARNING (abstract->context, "Invalid profile end marker detected!");
			length = RB_LOGBOOK_SIZE_FULL;
		} else if (length == RB_LOGBOOK_SIZE_FULL + 2) {
			// A profile containing only the 2 byte end-of-profile
			// marker is considered a valid empty profile.
		} else if (length < RB_LOGBOOK_SIZE_FULL + 5 ||
			array_uint24_le (profile + RB_LOGBOOK_SIZE_FULL) + delta != array_uint24_le (profile + HDR_FULL_LENGTH)) {
			// If there is more data available, then there should be a
			// valid profile header containing a length matching the
			// length in the dive header.
			WARNING (abstract->context, "Invalid profile header detected.");
			length = RB_LOGBOOK_SIZE_FULL;
		}

		if (callback && !callback (profile, length, profile + HDR_FULL_SUMMARY, sizeof (device->fingerprint), userdata))
			break;
	}

	free (profile);
	free (header);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	unsigned char packet[6] = {
		datetime->hour, datetime->minute, datetime->second,
		datetime->month, datetime->day, datetime->year - 2000};
	rc = hw_ostc3_transfer (device, NULL, CLOCK, packet, sizeof (packet), NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_display (dc_device_t *abstract, const char *text)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_DISPLAY] = {0};
	if (hw_ostc3_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, DISPLAY, packet, sizeof (packet), NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc3_device_customtext (dc_device_t *abstract, const char *text)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Pad the data packet with spaces.
	unsigned char packet[SZ_CUSTOMTEXT] = {0};
	if (hw_ostc3_strncpy (packet, sizeof (packet), text) != 0) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, CUSTOMTEXT, packet, sizeof (packet), NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_read (dc_device_t *abstract, unsigned int config, unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (device->hardware == OSTC4 ? size != SZ_CONFIG : size > SZ_CONFIG) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	unsigned char command[1] = {config};
	rc = hw_ostc3_transfer (device, NULL, READ, command, sizeof (command), data, size, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_write (dc_device_t *abstract, unsigned int config, const unsigned char data[], unsigned int size)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (device->hardware == OSTC4 ? size != SZ_CONFIG : size > SZ_CONFIG) {
		ERROR (abstract->context, "Invalid parameter specified.");
		return DC_STATUS_INVALIDARGS;
	}

	// Send the command.
	unsigned char command[SZ_CONFIG + 1] = {config};
	memcpy(command + 1, data, size);
	rc = hw_ostc3_transfer (device, NULL, WRITE, command, size + 1, NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

dc_status_t
hw_ostc3_device_config_reset (dc_device_t *abstract)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	dc_status_t rc = hw_ostc3_device_init (device, DOWNLOAD);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Send the command.
	rc = hw_ostc3_transfer (device, NULL, RESET, NULL, 0, NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}

// This is a variant of fletcher16 with a 16 bit sum instead of an 8 bit sum,
// and modulo 2^16 instead of 2^16-1
static unsigned int
hw_ostc3_firmware_checksum (const unsigned char data[], unsigned int size)
{
	unsigned short low = 0;
	unsigned short high = 0;
	for (unsigned int i = 0; i < size; i++) {
		low  += data[i];
		high += low;
	}
	return (((unsigned int)high) << 16) + low;
}

static dc_status_t
hw_ostc3_firmware_readline (FILE *fp, dc_context_t *context, unsigned int addr, unsigned char data[], unsigned int size)
{
	unsigned char ascii[39];
	unsigned char faddr_byte[3];
	unsigned int faddr = 0;
	size_t n = 0;

	if (size > 16) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Read the start code.
	while (1) {
		n = fread (ascii, 1, 1, fp);
		if (n != 1) {
			ERROR (context, "Failed to read the start code.");
			return DC_STATUS_IO;
		}

		if (ascii[0] == ':')
			break;

		// Ignore CR and LF characters.
		if (ascii[0] != '\n' && ascii[0] != '\r') {
			ERROR (context, "Unexpected character (0x%02x).", ascii[0]);
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Read the payload.
	n = fread (ascii + 1, 1, 6 + size * 2, fp);
	if (n != 6 + size * 2) {
		ERROR (context, "Failed to read the data.");
		return DC_STATUS_IO;
	}

	// Convert the address to binary representation.
	if (array_convert_hex2bin(ascii + 1, 6, faddr_byte, sizeof(faddr_byte)) != 0) {
		ERROR (context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the address.
	faddr = array_uint24_be (faddr_byte);
	if (faddr != addr) {
		ERROR (context, "Unexpected address (0x%06x, 0x%06x).", faddr, addr);
		return DC_STATUS_DATAFORMAT;
	}

	// Convert the payload to binary representation.
	if (array_convert_hex2bin (ascii + 1 + 6, size * 2, data, size) != 0) {
		ERROR (context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_firmware_readfile3 (hw_ostc3_firmware_t *firmware, dc_context_t *context, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	FILE *fp = NULL;
	unsigned char iv[16] = {0};
	unsigned char tmpbuf[16] = {0};
	unsigned char encrypted[16] = {0};
	unsigned int bytes = 0, addr = 0;
	unsigned char checksum[4];

	if (firmware == NULL) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Initialize the buffers.
	memset (firmware->data, 0xFF, sizeof (firmware->data));
	firmware->checksum = 0;

	fp = fopen (filename, "rb");
	if (fp == NULL) {
		ERROR (context, "Failed to open the file.");
		return DC_STATUS_IO;
	}

	rc = hw_ostc3_firmware_readline (fp, context, 0, iv, sizeof(iv));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to parse header.");
		fclose (fp);
		return rc;
	}
	bytes += 16;

	// Load the iv for AES-FCB-mode
	AES128_ECB_encrypt (iv, ostc3_key, tmpbuf);

	for (addr = 0; addr < SZ_FIRMWARE; addr += 16, bytes += 16) {
		rc = hw_ostc3_firmware_readline (fp, context, bytes, encrypted, sizeof(encrypted));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to parse file data.");
			fclose (fp);
			return rc;
		}

		// Decrypt AES-FCB data
		for (unsigned int i = 0; i < 16; i++)
			firmware->data[addr + i] = encrypted[i] ^ tmpbuf[i];

		// Run the next round of encryption
		AES128_ECB_encrypt (encrypted, ostc3_key, tmpbuf);
	}

	// This file format contains a tail with the checksum in
	rc = hw_ostc3_firmware_readline (fp, context, bytes, checksum, sizeof(checksum));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to parse file tail.");
		fclose (fp);
		return rc;
	}

	fclose (fp);

	unsigned int csum1 = array_uint32_le (checksum);
	unsigned int csum2 = hw_ostc3_firmware_checksum (firmware->data, sizeof(firmware->data));
	if (csum1 != csum2) {
		ERROR (context, "Failed to verify file checksum.");
		return DC_STATUS_DATAFORMAT;
	}

	firmware->checksum = csum1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_firmware_readfile4 (dc_buffer_t *buffer, dc_context_t *context, const char *filename)
{
	FILE *fp = NULL;

	if (buffer == NULL) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Open the file.
	fp = fopen (filename, "rb");
	if (fp == NULL) {
		ERROR (context, "Failed to open the file.");
		return DC_STATUS_IO;
	}

	// Read the entire file into the buffer.
	size_t n = 0;
	unsigned char block[1024] = {0};
	while ((n = fread (block, 1, sizeof (block), fp)) > 0) {
		if (!dc_buffer_append (buffer, block, n)) {
			ERROR (context, "Insufficient buffer space available.");
			fclose (fp);
			return DC_STATUS_NOMEMORY;
		}
	}

	// Close the file.
	fclose (fp);

	// Verify the minimum size.
	size_t size = dc_buffer_get_size (buffer);
	if (size < 4) {
		ERROR (context, "Invalid file size.");
		return DC_STATUS_DATAFORMAT;

	}

	// Verify the checksum.
	const unsigned char *data = dc_buffer_get_data (buffer);
	unsigned int csum1 = array_uint32_le (data + size - 4);
	unsigned int csum2 = hw_ostc3_firmware_checksum (data, size - 4);
	if (csum1 != csum2) {
		ERROR (context, "Failed to verify file checksum.");
		return DC_STATUS_DATAFORMAT;
	}

	// Remove the checksum.
	dc_buffer_slice (buffer, 0, size - 4);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_firmware_erase (hw_ostc3_device_t *device, unsigned int addr, unsigned int size)
{
	// Convert size to number of pages, rounded up.
	unsigned char blocks = ((size + SZ_FIRMWARE_BLOCK - 1) / SZ_FIRMWARE_BLOCK);

	// Estimate the required delay. Erasing a 4K flash memory page
	// takes around 25 milliseconds.
	unsigned int delay = blocks * 25;

	// Erase just the needed pages.
	unsigned char buffer[4];
	array_uint24_be_set (buffer, addr);
	buffer[3] = blocks;

	return hw_ostc3_transfer (device, NULL, S_ERASE, buffer, sizeof (buffer), NULL, 0, NULL, delay);
}

static dc_status_t
hw_ostc3_firmware_block_read (hw_ostc3_device_t *device, unsigned int addr, unsigned char block[], unsigned int block_size)
{
	unsigned char buffer[6];
	array_uint24_be_set (buffer, addr);
	array_uint24_be_set (buffer + 3, block_size);

	return hw_ostc3_transfer (device, NULL, S_BLOCK_READ, buffer, sizeof (buffer), block, block_size, NULL, NODELAY);
}

static dc_status_t
hw_ostc3_firmware_block_write1 (hw_ostc3_device_t *device, unsigned int addr, const unsigned char block[], unsigned int block_size)
{
	unsigned char buffer[3 + SZ_FIRMWARE_BLOCK];

	// We currently only support writing max SZ_FIRMWARE_BLOCK sized blocks.
	if (block_size > SZ_FIRMWARE_BLOCK)
		return DC_STATUS_INVALIDARGS;

	array_uint24_be_set (buffer, addr);
	memcpy (buffer + 3, block, block_size);

	return hw_ostc3_transfer (device, NULL, S_BLOCK_WRITE, buffer, 3 + block_size, NULL, 0, NULL, TIMEOUT);
}

static dc_status_t
hw_ostc3_firmware_block_write2 (hw_ostc3_device_t *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if ((address % SZ_FIRMWARE_BLOCK2 != 0) ||
		(size % SZ_FIRMWARE_BLOCK2 != 0)) {
		return DC_STATUS_INVALIDARGS;
	}

	unsigned int nbytes = 0;
	while (nbytes < size) {
		unsigned char buffer[3 + SZ_FIRMWARE_BLOCK2];
		array_uint24_be_set (buffer, address);
		memcpy (buffer + 3, data + nbytes, SZ_FIRMWARE_BLOCK2);

		status = hw_ostc3_transfer (device, NULL, S_BLOCK_WRITE2, buffer, sizeof(buffer), NULL, 0, NULL, NODELAY);
		if (status != DC_STATUS_SUCCESS) {
			return status;
		}

		address += SZ_FIRMWARE_BLOCK2;
		nbytes += SZ_FIRMWARE_BLOCK2;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_firmware_block_write (hw_ostc3_device_t *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	// Support for the S_BLOCK_WRITE2 command is only available since the
	// hwOS Tech firmware v3.09 and the hwOS Sport firmware v10.64.
	if ((device->firmware < OSTC3FW(3,9)) ||
		(device->firmware >= OSTC3FW(10,0) && device->firmware < OSTC3FW(10,64))) {
		return hw_ostc3_firmware_block_write1 (device, address, data, size);
	} else {
		return hw_ostc3_firmware_block_write2 (device, address, data, size);
	}
}

static dc_status_t
hw_ostc3_firmware_upgrade (dc_device_t *abstract, unsigned int checksum)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);
	unsigned char buffer[5];
	array_uint32_le_set (buffer, checksum);

	// Compute a one byte checksum, so the device can validate the firmware image.
	buffer[4] = 0x55;
	for (unsigned int i = 0; i < 4; i++) {
		buffer[4] ^= buffer[i];
		buffer[4]  = (buffer[4]<<1 | buffer[4]>>7);
	}

	rc = hw_ostc3_transfer (device, NULL, S_UPGRADE, buffer, sizeof (buffer), NULL, 0, NULL, NODELAY);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to send flash firmware command");
		return rc;
	}

	// Now the device resets, and if everything is well, it reprograms.
	device->state = REBOOTING;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc3_device_fwupdate3 (dc_device_t *abstract, const char *filename)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Enable progress notifications.
	// load, erase, upload FZ, verify FZ, reprogram
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = 3 + SZ_FIRMWARE * 2 / SZ_FIRMWARE_BLOCK;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the firmware data.
	hw_ostc3_firmware_t *firmware = (hw_ostc3_firmware_t *) malloc (sizeof (hw_ostc3_firmware_t));
	if (firmware == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Read the hex file.
	rc = hw_ostc3_firmware_readfile3 (firmware, context, filename);
	if (rc != DC_STATUS_SUCCESS) {
		free (firmware);
		return rc;
	}

	// Device open and firmware loaded
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	hw_ostc3_device_display (abstract, " Erasing FW...");

	rc = hw_ostc3_firmware_erase (device, FIRMWARE_AREA, SZ_FIRMWARE);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to erase old firmware");
		free (firmware);
		return rc;
	}

	// Memory erased
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	hw_ostc3_device_display (abstract, " Uploading...");

	for (unsigned int len = 0; len < SZ_FIRMWARE; len += SZ_FIRMWARE_BLOCK) {
		char status[SZ_DISPLAY + 1]; // Status message on the display
		dc_platform_snprintf (status, sizeof(status), " Uploading %2d%%", (100 * len) / SZ_FIRMWARE);
		hw_ostc3_device_display (abstract, status);

		rc = hw_ostc3_firmware_block_write (device, FIRMWARE_AREA + len, firmware->data + len, SZ_FIRMWARE_BLOCK);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to write block to device");
			free(firmware);
			return rc;
		}
		// One block uploaded
		progress.current++;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	hw_ostc3_device_display (abstract, " Verifying...");

	for (unsigned int len = 0; len < SZ_FIRMWARE; len += SZ_FIRMWARE_BLOCK) {
		unsigned char block[SZ_FIRMWARE_BLOCK];
		char status[SZ_DISPLAY + 1]; // Status message on the display
		dc_platform_snprintf (status, sizeof(status), " Verifying %2d%%", (100 * len) / SZ_FIRMWARE);
		hw_ostc3_device_display (abstract, status);

		rc = hw_ostc3_firmware_block_read (device, FIRMWARE_AREA + len, block, sizeof (block));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to read block.");
			free (firmware);
			return rc;
		}
		if (memcmp (firmware->data + len, block, sizeof (block)) != 0) {
			ERROR (context, "Failed verify.");
			hw_ostc3_device_display (abstract, " Verify FAILED");
			free (firmware);
			return DC_STATUS_PROTOCOL;
		}
		// One block verified
		progress.current++;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
	}

	hw_ostc3_device_display (abstract, " Programming...");

	rc = hw_ostc3_firmware_upgrade (abstract, firmware->checksum);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to start programing");
		free (firmware);
		return rc;
	}

	// Programing done!
	progress.current++;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	free (firmware);

	// Finished!
	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_device_fwupdate4 (dc_device_t *abstract, const char *filename)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;
	dc_context_t *context = (abstract ? abstract->context : NULL);

	// Allocate memory for the firmware data.
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL) {
		ERROR (context, "Failed to allocate memory.");
		status = DC_STATUS_NOMEMORY;
		goto error;
	}

	// Read the firmware file.
	status = hw_ostc3_firmware_readfile4 (buffer, context, filename);
	if (status != DC_STATUS_SUCCESS) {
		goto error;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = dc_buffer_get_size (buffer);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Cache the pointer and size.
	const unsigned char *data = dc_buffer_get_data (buffer);
	unsigned int size = dc_buffer_get_size (buffer);

	unsigned int offset = 0;
	while (offset + 4 <= size) {
		// Get the length of the firmware blob.
		unsigned int length = array_uint32_be(data + offset) + 20;
		if (offset + length > size) {
			status = DC_STATUS_DATAFORMAT;
			goto error;
		}

		// Get the blob type.
		unsigned char type = data[offset + 4];

		// Estimate the required delay.
		// After uploading the firmware blob, the device writes the data
		// to flash memory. Since this takes a significant amount of
		// time, the ready byte is delayed. Therefore, the standard
		// timeout is no longer sufficient. The delays are estimated
		// based on actual measurements of the delay per byte.
		unsigned int usecs = length;
		if (type == 0xFF) {
			// Firmware
			usecs *= 50;
		} else if (type == 0xFE) {
			// RTE
			usecs *= 500;
		} else {
			// Fonts
			usecs *= 25;
		}

		// Read the firmware version info.
		unsigned char fwinfo[SZ_FWINFO] = {0};
		status = hw_ostc3_transfer (device, NULL, S_FWINFO,
			data + offset + 4, 1, fwinfo, sizeof(fwinfo), NULL, NODELAY);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the firmware info.");
			goto error;
		}

		// Upload the firmware blob.
		// The update is skipped if the two versions are already
		// identical, or if the blob is not present on the device.
		if (memcmp(data + offset + 12, fwinfo, sizeof(fwinfo)) != 0 &&
			!array_isequal(fwinfo, sizeof(fwinfo), 0xFF))
		{
			status = hw_ostc3_transfer (device, &progress, S_UPLOAD,
				data + offset, length, NULL, 0, NULL, usecs / 1000);
			if (status != DC_STATUS_SUCCESS) {
				goto error;
			}
		} else {
			// Update and emit a progress event.
			progress.current += length;
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
		}

		offset += length;
	}

error:
	dc_buffer_free (buffer);
	return status;
}

dc_status_t
hw_ostc3_device_fwupdate (dc_device_t *abstract, const char *filename)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Make sure the device is in service mode.
	status = hw_ostc3_device_init (device, SERVICE);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	if (device->hardware == OSTC4) {
		return hw_ostc3_device_fwupdate4 (abstract, filename);
	} else {
		return hw_ostc3_device_fwupdate3 (abstract, filename);
	}
}

static dc_status_t
hw_ostc3_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if ((address % SZ_FIRMWARE_BLOCK != 0) ||
		(size % SZ_FIRMWARE_BLOCK != 0)) {
		ERROR (abstract->context, "Address or size not aligned to the page size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Make sure the device is in service mode.
	status = hw_ostc3_device_init (device, SERVICE);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	if (device->hardware == OSTC4) {
		return DC_STATUS_UNSUPPORTED;
	}

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Read a memory page.
		status = hw_ostc3_firmware_block_read (device, address + nbytes, data + nbytes, SZ_FIRMWARE_BLOCK);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read block.");
			return status;
		}

		nbytes += SZ_FIRMWARE_BLOCK;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	if ((address % SZ_FIRMWARE_BLOCK != 0) ||
		(size % SZ_FIRMWARE_BLOCK != 0)) {
		ERROR (abstract->context, "Address or size not aligned to the page size!");
		return DC_STATUS_INVALIDARGS;
	}

	// Make sure the device is in service mode.
	status = hw_ostc3_device_init (device, SERVICE);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	if (device->hardware == OSTC4) {
		return DC_STATUS_UNSUPPORTED;
	}

	// Erase the memory pages.
	status = hw_ostc3_firmware_erase (device, address, size);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to erase blocks.");
		return status;
	}

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Write a memory page.
		status = hw_ostc3_firmware_block_write (device, address + nbytes, data + nbytes, SZ_FIRMWARE_BLOCK);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to write block.");
			return status;
		}

		nbytes += SZ_FIRMWARE_BLOCK;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc3_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	hw_ostc3_device_t *device = (hw_ostc3_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Make sure the device is in service mode
	dc_status_t rc = hw_ostc3_device_init (device, SERVICE);
	if (rc != DC_STATUS_SUCCESS) {
		return rc;
	}

	if (device->hardware == OSTC4) {
		return DC_STATUS_UNSUPPORTED;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.firmware = device->firmware;
	devinfo.serial = device->serial;
	if (device->hardware != UNKNOWN) {
		devinfo.model = device->hardware;
	} else {
		// Fallback to the serial number.
		if (devinfo.serial > 10000)
			devinfo.model = SPORT;
		else
			devinfo.model = OSTC3;
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	unsigned int nbytes = 0;
	while (nbytes < SZ_MEMORY) {
		// packet size. Can be almost arbitrary size.
		unsigned int len = SZ_FIRMWARE_BLOCK;

		// Read a block
		rc = hw_ostc3_firmware_block_read (device, nbytes, data + nbytes, len);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read block.");
			return rc;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}
