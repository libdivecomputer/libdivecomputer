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

#include "mares_iconhd.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "rbstream.h"
#include "platform.h"
#include "packet.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_iconhd_device_vtable)

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

#define MATRIX    0x0F
#define SMART      0x000010
#define SMARTAPNEA 0x010010
#define ICONHD    0x14
#define ICONHDNET 0x15
#define PUCKPRO   0x18
#define NEMOWIDE2 0x19
#define GENIUS    0x1C
#define PUCK2     0x1F
#define QUADAIR   0x23
#define SMARTAIR  0x24
#define QUAD      0x29
#define HORIZON   0x2C
#define PUCKAIR2  0x2D
#define SIRIUS    0x2F
#define QUADCI    0x31
#define PUCK4     0x35

#define ISSMART(model) ( \
	(model) == SMART || \
	(model) == SMARTAPNEA || \
	(model) == SMARTAIR)

#define ISGENIUS(model) ( \
	(model) == GENIUS || \
	(model) == HORIZON || \
	(model) == PUCKAIR2 || \
	(model) == SIRIUS || \
	(model) == QUADCI || \
	(model) == PUCK4)

#define ISSIRIUS(model) ( \
	(model) == PUCKAIR2 || \
	(model) == SIRIUS || \
	(model) == QUADCI || \
	(model) == PUCK4)

#define MAXRETRIES 4

#define MAXPACKET 244

#define FIXED    0
#define VARIABLE 1

#define ACK 0xAA
#define END 0xEA
#define XOR 0xA5

#define CMD_VERSION   0xC2
#define CMD_FLASHSIZE 0xB3
#define CMD_READ      0xE7
#define CMD_OBJ_INIT  0xBF
#define CMD_OBJ_EVEN  0xAC
#define CMD_OBJ_ODD   0xFE

#define OBJ_DEVICE        0x2000
#define OBJ_DEVICE_MODEL  0x02
#define OBJ_DEVICE_SERIAL 0x04
#define OBJ_LOGBOOK       0x2008
#define OBJ_LOGBOOK_COUNT 0x01
#define OBJ_DIVE          0x3000
#define OBJ_DIVE_HEADER   0x02
#define OBJ_DIVE_DATA     0x03

#define AIR       0
#define GAUGE     1
#define NITROX    2
#define FREEDIVE  3

typedef struct mares_iconhd_layout_t {
	unsigned int memsize;
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
} mares_iconhd_layout_t;

typedef struct mares_iconhd_model_t {
	unsigned char name[16 + 1];
	unsigned int id;
} mares_iconhd_model_t;

typedef struct mares_iconhd_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	const mares_iconhd_layout_t *layout;
	unsigned char fingerprint[10];
	unsigned int fingerprint_size;
	unsigned char version[140];
	unsigned int model;
	unsigned int packetsize;
	unsigned int ble;
} mares_iconhd_device_t;

static dc_status_t mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t mares_iconhd_device_close (dc_device_t *abstract);

static const dc_device_vtable_t mares_iconhd_device_vtable = {
	sizeof(mares_iconhd_device_t),
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_device_set_fingerprint, /* set_fingerprint */
	mares_iconhd_device_read, /* read */
	NULL, /* write */
	mares_iconhd_device_dump, /* dump */
	mares_iconhd_device_foreach, /* foreach */
	NULL, /* timesync */
	mares_iconhd_device_close /* close */
};

static const mares_iconhd_layout_t mares_iconhd_layout = {
	0x100000, /* memsize */
	0x00A000, /* rb_profile_begin */
	0x100000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_iconhdnet_layout = {
	0x100000, /* memsize */
	0x00E000, /* rb_profile_begin */
	0x100000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_genius_layout = {
	0x1000000, /* memsize */
	0x0100000, /* rb_profile_begin */
	0x1000000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_matrix_layout = {
	0x40000, /* memsize */
	0x0A000, /* rb_profile_begin */
	0x3E000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_nemowide2_layout = {
	0x40000, /* memsize */
	0x0A000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
};

static unsigned int
mares_iconhd_get_model (mares_iconhd_device_t *device)
{
	const mares_iconhd_model_t models[] = {
		{"Matrix",      MATRIX},
		{"Smart",       SMART},
		{"Smart Apnea", SMARTAPNEA},
		{"Icon HD",     ICONHD},
		{"Icon AIR",    ICONHDNET},
		{"Puck Pro",    PUCKPRO},
		{"Nemo Wide 2", NEMOWIDE2},
		{"Genius",      GENIUS},
		{"Puck 2",      PUCK2},
		{"Quad Air",    QUADAIR},
		{"Smart Air",   SMARTAIR},
		{"Quad",        QUAD},
		{"Horizon",     HORIZON},
		{"Puck Air 2",  PUCKAIR2},
		{"Sirius",      SIRIUS},
		{"Quad Ci",     QUADCI},
		{"Puck4",       PUCK4},
		{"Puck Lite",   PUCK4},
	};

	// Check the product name in the version packet against the list
	// with valid names, and return the corresponding model number.
	unsigned int model = 0;
	for (unsigned int i = 0; i < C_ARRAY_SIZE(models); ++i) {
		if (memcmp (device->version + 0x46, models[i].name, sizeof (models[i].name) - 1) == 0) {
			model = models[i].id;
			break;
		}
	}

	return model;
}

static dc_status_t
mares_iconhd_packet_fixed (mares_iconhd_device_t *device,
	unsigned char cmd,
	const unsigned char data[], unsigned int size,
	unsigned char answer[], unsigned int asize,
	unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command header to the dive computer.
	const unsigned char command[2] = {
		cmd, cmd ^ XOR,
	};
	status = dc_iostream_write (device->iostream, command, sizeof(command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command header.");
		return status;
	}

	// Send the command payload to the dive computer.
	if (size && transport == DC_TRANSPORT_BLE) {
		status = dc_iostream_write (device->iostream, data, size, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command data.");
			return status;
		}
	}

	// Receive the header byte.
	while (1) {
		unsigned char header[1] = {0};
		status = dc_iostream_read (device->iostream, header, sizeof (header), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet header.");
			return status;
		}

		if (header[0] == ACK)
			break;

		WARNING (abstract->context, "Unexpected packet header byte (%02x).", header[0]);
	}

	// Send the command payload to the dive computer.
	if (size && transport != DC_TRANSPORT_BLE) {
		status = dc_iostream_write (device->iostream, data, size, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command data.");
			return status;
		}
	}

	// Read the packet.
	status = dc_iostream_read (device->iostream, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet data.");
		return status;
	}

	// Receive the trailer byte.
	unsigned char trailer[1] = {0};
	status = dc_iostream_read (device->iostream, trailer, sizeof (trailer), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet trailer.");
		return status;
	}

	// Verify the trailer byte.
	if (trailer[0] != END) {
		ERROR (abstract->context, "Unexpected packet trailer byte (%02x).", trailer[0]);
		return DC_STATUS_PROTOCOL;
	}

	if (actual) {
		*actual = asize;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_packet_variable (mares_iconhd_device_t *device,
	unsigned char cmd,
	const unsigned char data[], unsigned int size,
	unsigned char answer[], unsigned int asize,
	unsigned int *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET] = {0};
	size_t length = 0;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command header to the dive computer.
	const unsigned char command[2] = {
		cmd, cmd ^ XOR,
	};
	status = dc_iostream_write (device->iostream, command, sizeof(command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command header.");
		return status;
	}

	// Read either the entire data packet (if there is no command data to send),
	// or only the header byte (if there is also command data to send).
	status = dc_iostream_read (device->iostream, packet, sizeof (packet), &length);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the packet header.");
		return status;
	}

	if (size) {
		// Send the command payload to the dive computer.
		status = dc_iostream_write (device->iostream, data, size, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command data.");
			return status;
		}

		// Read the data packet.
		size_t len = 0;
		status = dc_iostream_read (device->iostream, packet + length, sizeof (packet) - length, &len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the packet data.");
			return status;
		}

		length += len;
	}

	if (length < 2 || length - 2 > asize) {
		ERROR (abstract->context, "Unexpected packet length (" DC_PRINTF_SIZE ").", length);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the header byte.
	if (packet[0] != ACK) {
		ERROR (abstract->context, "Unexpected packet header byte (%02x).", packet[0]);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the trailer byte.
	if (packet[length - 1] != END) {
		ERROR (abstract->context, "Unexpected packet trailer byte (%02x).", packet[length - 1]);
		return DC_STATUS_PROTOCOL;
	}

	if (actual == NULL) {
		// Verify the actual length.
		if (length - 2 != asize) {
			ERROR (abstract->context, "Unexpected packet length (" DC_PRINTF_SIZE ").", length - 2);
			return DC_STATUS_PROTOCOL;
		}
	} else {
		// Return the actual length.
		*actual = length - 2;
	}

	memcpy (answer, packet + 1, length - 2);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_packet (mares_iconhd_device_t *device,
	unsigned char cmd,
	const unsigned char data[], unsigned int size,
	unsigned char answer[], unsigned int asize,
	unsigned int *actual)
{
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (transport == DC_TRANSPORT_BLE && device->ble == VARIABLE) {
		return mares_iconhd_packet_variable (device, cmd, data, size, answer, asize, actual);
	} else {
		return mares_iconhd_packet_fixed (device, cmd, data, size, answer, asize, actual);
	}
}

static dc_status_t
mares_iconhd_transfer (mares_iconhd_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size, unsigned char answer[], unsigned int asize, unsigned int *actual)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = mares_iconhd_packet (device, cmd, data, size, answer, asize, actual)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Discard any garbage bytes.
		dc_iostream_sleep (device->iostream, 1000);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_read_object(mares_iconhd_device_t *device, dc_event_progress_t *progress, dc_buffer_t *buffer, unsigned int index, unsigned int subindex)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);
	const unsigned int maxpacket = (transport == DC_TRANSPORT_BLE) ?
		(device->ble == VARIABLE ? MAXPACKET - 3 : 124) :
		504;

	// Update and emit a progress event.
	unsigned int initial = 0;
	if (progress) {
		initial = progress->current;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	// Transfer the init packet.
	unsigned char rsp_init[16];
	unsigned char cmd_init[16] = {
		0x40,
		(index >> 0) & 0xFF,
		(index >> 8) & 0xFF,
		subindex & 0xFF
	};
	memset (cmd_init + 6, 0x00, sizeof(cmd_init) - 6);
	status = mares_iconhd_transfer (device, CMD_OBJ_INIT, cmd_init, sizeof (cmd_init), rsp_init, sizeof (rsp_init), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to transfer the init packet.");
		return status;
	}

	// Verify the packet header.
	if (memcmp (cmd_init + 1, rsp_init + 1, 3) != 0) {
		ERROR (abstract->context, "Unexpected packet header.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0, size = 0;
	if (rsp_init[0] == 0x41) {
		// A large (and variable size) payload is split into multiple
		// data packets. The first packet contains only the total size
		// of the payload.
		size = array_uint32_le (rsp_init + 4);
	} else if (rsp_init[0] == 0x42) {
		// A short (and fixed size) payload is embedded into the first
		// data packet.
		size = sizeof(rsp_init) - 4;

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, rsp_init + 4, sizeof(rsp_init) - 4)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += sizeof(rsp_init) - 4;
	} else {
		ERROR (abstract->context, "Unexpected packet type (%02x).", rsp_init[0]);
		return DC_STATUS_PROTOCOL;
	}

	// Update and emit a progress event.
	if (progress) {
		progress->current = initial + STEP (nbytes, size);
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	unsigned int npackets = 0;
	while (nbytes < size) {
		// Get the command byte.
		unsigned char toggle = npackets % 2;
		unsigned char cmd = toggle == 0 ? CMD_OBJ_EVEN : CMD_OBJ_ODD;

		// Get the packet size.
		unsigned int len = size - nbytes;
		if (len > maxpacket) {
			len = maxpacket;
		}

		// Transfer the segment packet.
		unsigned int length = 0;
		unsigned char rsp_segment[1 + 504];
		status = mares_iconhd_transfer (device, cmd, NULL, 0, rsp_segment, len + 1, &length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to transfer the segment packet.");
			return status;
		}

		if (length < 1) {
			ERROR (abstract->context, "Unexpected packet length (%u).", length);
			return DC_STATUS_PROTOCOL;
		}

		length--;

		// Verify the packet header.
		if ((rsp_segment[0] & 0xF0) >> 4 != toggle) {
			ERROR (abstract->context, "Unexpected packet header (%02x).", rsp_segment[0]);
			return DC_STATUS_PROTOCOL;
		}

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, rsp_segment + 1, length)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += length;
		npackets++;

		// Update and emit the progress events.
		if (progress) {
			progress->current = initial + STEP (nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return status;
}

dc_status_t
mares_iconhd_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = NULL;
	dc_transport_t transport = dc_iostream_get_transport (iostream);

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mares_iconhd_device_t *) dc_device_allocate (context, &mares_iconhd_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->layout = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->fingerprint_size = sizeof (device->fingerprint);
	memset (device->version, 0, sizeof (device->version));
	device->model = 0;
	device->packetsize = 0;
	device->ble = ISSIRIUS(model) ? VARIABLE : FIXED;

	// Create the packet stream.
	if (transport == DC_TRANSPORT_BLE && device->ble == FIXED) {
		status = dc_packet_open (&device->iostream, context, iostream, 244, 20);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (context, "Failed to create the packet stream.");
			goto error_free;
		}
	} else {
		device->iostream = iostream;
	}

	// Set the serial communication protocol (115200 8E1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_EVEN, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free_iostream;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free_iostream;
	}

	// Clear the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_free_iostream;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free_iostream;
	}

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Send the version command.
	status = mares_iconhd_transfer (device, CMD_VERSION, NULL, 0,
		device->version, sizeof (device->version), NULL);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free_iostream;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Version", device->version, sizeof (device->version));

	// Autodetect the model using the version packet.
	device->model = mares_iconhd_get_model (device);

	// Read the size of the flash memory.
	unsigned int memsize = 0;
	if (device->model == QUAD) {
		unsigned char rsp_flash[4] = {0};
		status = mares_iconhd_transfer (device, CMD_FLASHSIZE, NULL, 0, rsp_flash, sizeof (rsp_flash), NULL);
		if (status != DC_STATUS_SUCCESS) {
			WARNING (context, "Failed to read the flash memory size.");
		} else {
			memsize = array_uint32_le (rsp_flash);
			DEBUG (context, "Flash memory size is %u bytes.", memsize);
		}
	}

	// Load the correct memory layout.
	switch (device->model) {
	case MATRIX:
		device->layout = &mares_matrix_layout;
		device->packetsize = 256;
		break;
	case PUCKPRO:
	case PUCK2:
	case NEMOWIDE2:
	case SMART:
	case SMARTAPNEA:
		device->layout = &mares_nemowide2_layout;
		device->packetsize = 256;
		break;
	case QUAD:
		if (memsize > 0x40000) {
			device->layout = &mares_iconhd_layout;
		} else {
			device->layout = &mares_nemowide2_layout;
		}
		device->packetsize = 256;
		break;
	case QUADAIR:
	case SMARTAIR:
		device->layout = &mares_iconhdnet_layout;
		device->packetsize = 256;
		break;
	case GENIUS:
	case HORIZON:
	case PUCKAIR2:
	case SIRIUS:
	case QUADCI:
	case PUCK4:
		device->layout = &mares_genius_layout;
		device->packetsize = 4096;
		device->fingerprint_size = 4;
		break;
	case ICONHDNET:
		device->layout = &mares_iconhdnet_layout;
		device->packetsize = 4096;
		break;
	case ICONHD:
	default:
		device->layout = &mares_iconhd_layout;
		device->packetsize = 4096;
		break;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;


error_free_iostream:
	if (transport == DC_TRANSPORT_BLE && device->ble == FIXED) {
		dc_iostream_close (device->iostream);
	}
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
mares_iconhd_device_close (dc_device_t *abstract)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	// Close the packet stream.
	if (transport == DC_TRANSPORT_BLE && device->ble == FIXED) {
		return dc_iostream_close (device->iostream);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	if (size && size != device->fingerprint_size)
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, device->fingerprint_size);
	else
		memset (device->fingerprint, 0, device->fingerprint_size);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > device->packetsize)
			len = device->packetsize;

		// Read the packet.
		unsigned char command[] = {
			(address      ) & 0xFF,
			(address >>  8) & 0xFF,
			(address >> 16) & 0xFF,
			(address >> 24) & 0xFF,
			(len      ) & 0xFF,
			(len >>  8) & 0xFF,
			(len >> 16) & 0xFF,
			(len >> 24) & 0xFF};
		rc = mares_iconhd_transfer (device, CMD_READ, command, sizeof (command), data, len, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return rc;
}


static dc_status_t
mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Download the memory dump.
	status = device_dump_read (abstract, 0, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), device->packetsize);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (data + 0x0C);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return status;
}

static dc_status_t
mares_iconhd_device_foreach_raw (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	const mares_iconhd_layout_t *layout = device->layout;

	// Read the serial number.
	unsigned char serial[4] = {0};
	rc = mares_iconhd_device_read (abstract, 0x0C, serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory.");
		return rc;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = layout->rb_profile_end - layout->rb_profile_begin;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Get the model code.
	unsigned int model = device->model;

	// Get the corresponding dive header size.
	unsigned int header = 0x5C;
	if (model == ICONHDNET)
		header = 0x80;
	else if (model == QUADAIR)
		header = 0x84;
	else if (model == SMART || model == SMARTAIR)
		header = 4; // Type and number of samples only!
	else if (model == SMARTAPNEA)
		header = 6; // Type and number of samples only!

	// Get the end of the profile ring buffer.
	unsigned int eop = 0;
	const unsigned int config[] = {0x2001, 0x3001};
	for (unsigned int i = 0; i < sizeof (config) / sizeof (*config); ++i) {
		// Read the pointer.
		unsigned char pointer[4] = {0};
		rc = mares_iconhd_device_read (abstract, config[i], pointer, sizeof (pointer));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the memory.");
			return rc;
		}

		// Update and emit a progress event.
		progress.maximum += sizeof (pointer);
		progress.current += sizeof (pointer);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		eop = array_uint32_le (pointer);
		if (eop != 0xFFFFFFFF)
			break;
	}
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		if (eop == 0xFFFFFFFF)
			return DC_STATUS_SUCCESS; // No dives available.
		ERROR (abstract->context, "Ringbuffer pointer out of range (0x%08x).", eop);
		return DC_STATUS_DATAFORMAT;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, 1, device->packetsize, layout->rb_profile_begin, layout->rb_profile_end, eop, DC_RBSTREAM_BACKWARD);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// Allocate memory for the dives.
	unsigned char *buffer = (unsigned char *) malloc (layout->rb_profile_end - layout->rb_profile_begin);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		dc_rbstream_free (rbstream);
		return DC_STATUS_NOMEMORY;
	}

	unsigned int offset = layout->rb_profile_end - layout->rb_profile_begin;
	while (offset >= header + 4) {
		// Read the first part of the dive header.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - header, header);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Get the number of samples in the profile data.
		unsigned int type = 0, nsamples = 0;
		if (ISSMART(model)) {
			type     = array_uint16_le (buffer + offset - header + 2);
			nsamples = array_uint16_le (buffer + offset - header + 0);
		} else {
			type     = array_uint16_le (buffer + offset - header + 0);
			nsamples = array_uint16_le (buffer + offset - header + 2);
		}
		if (nsamples == 0xFFFF || type == 0xFFFF)
			break;

		// Get the dive mode.
		unsigned int mode = type & 0x03;

		// Get the header/sample size and fingerprint offset.
		unsigned int headersize = 0x5C;
		unsigned int samplesize = 8;
		unsigned int fingerprint = 6;
		if (model == ICONHDNET) {
			headersize = 0x80;
			samplesize = 12;
		} else if (model == QUADAIR) {
			headersize = 0x84;
			samplesize = 12;
		} else if (model == SMART) {
			if (mode == FREEDIVE) {
				headersize = 0x2E;
				samplesize = 6;
				fingerprint = 0x20;
			} else {
				headersize = 0x5C;
				samplesize = 8;
				fingerprint = 2;
			}
		} else if (model == SMARTAPNEA) {
			headersize = 0x50;
			samplesize = 14;
			fingerprint = 0x40;
		} else if (model == SMARTAIR) {
			if (mode == FREEDIVE) {
				headersize = 0x30;
				samplesize = 6;
				fingerprint = 0x22;
			} else {
				headersize = 0x84;
				samplesize = 12;
				fingerprint = 2;
			}
		}
		if (offset < headersize)
			break;

		// Read the second part of the dive header.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - headersize, headersize - header);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Calculate the total number of bytes for this dive.
		// If the buffer does not contain that much bytes, we reached the
		// end of the ringbuffer. The current dive is incomplete (partially
		// overwritten with newer data), and processing should stop.
		unsigned int nbytes = 4 + headersize + nsamples * samplesize;
		if (model == ICONHDNET || model == QUADAIR || (model == SMARTAIR && mode != FREEDIVE)) {
			nbytes += (nsamples / 4) * 8;
		} else if (model == SMARTAPNEA) {
			unsigned int settings = array_uint16_le (buffer + offset - headersize + 0x1C);
			unsigned int divetime = array_uint32_le (buffer + offset - headersize + 0x24);
			unsigned int samplerate = 1 << ((settings >> 9) & 0x03);

			nbytes += divetime * samplerate * 2;
		}
		if (offset < nbytes)
			break;

		// Read the remainder of the dive.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - nbytes, nbytes - headersize);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Move to the start of the dive.
		offset -= nbytes;

		// Verify that the length that is stored in the profile data
		// equals the calculated length. If both values are different,
		// we assume we reached the last dive.
		unsigned int length = array_uint32_le (buffer + offset);
		if (length != nbytes)
			break;

		unsigned char *fp = buffer + offset + length - headersize + fingerprint;
		if (memcmp (fp, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			break;
		}

		if (callback && !callback (buffer + offset, length, fp, sizeof (device->fingerprint), userdata)) {
			break;
		}
	}

	dc_rbstream_free (rbstream);
	free (buffer);

	return rc;
}

static dc_status_t
mares_iconhd_device_foreach_object (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the dives.
	dc_buffer_t *buffer = dc_buffer_new (4096);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Read the model number.
	rc = mares_iconhd_read_object (device, NULL, buffer, OBJ_DEVICE, OBJ_DEVICE_MODEL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the model number.");
		dc_buffer_free (buffer);
		return rc;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Model", dc_buffer_get_data (buffer), dc_buffer_get_size (buffer));

	if (dc_buffer_get_size (buffer) < 4) {
		ERROR (abstract->context, "Unexpected number of bytes received (" DC_PRINTF_SIZE ").",
			dc_buffer_get_size (buffer));
		dc_buffer_free (buffer);
		return DC_STATUS_PROTOCOL;
	}

	unsigned int DC_ATTR_UNUSED model = array_uint32_le (dc_buffer_get_data (buffer));

	// Erase the buffer.
	dc_buffer_clear (buffer);

	// Read the serial number.
	rc = mares_iconhd_read_object (device, NULL, buffer, OBJ_DEVICE, OBJ_DEVICE_SERIAL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the serial number.");
		dc_buffer_free (buffer);
		return rc;
	}

	HEXDUMP (abstract->context, DC_LOGLEVEL_DEBUG, "Serial", dc_buffer_get_data (buffer), dc_buffer_get_size (buffer));

	if (dc_buffer_get_size (buffer) < 16) {
		ERROR (abstract->context, "Unexpected number of bytes received (" DC_PRINTF_SIZE ").",
			dc_buffer_get_size (buffer));
		dc_buffer_free (buffer);
		return DC_STATUS_PROTOCOL;
	}

	unsigned int serial = array_convert_str2num (dc_buffer_get_data (buffer) + 10, 6);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = serial;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Erase the buffer.
	dc_buffer_clear (buffer);

	// Read the number of dives.
	rc = mares_iconhd_read_object (device, NULL, buffer, OBJ_LOGBOOK, OBJ_LOGBOOK_COUNT);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the number of dives.");
		dc_buffer_free (buffer);
		return rc;
	}

	if (dc_buffer_get_size (buffer) < 2) {
		ERROR (abstract->context, "Unexpected number of bytes received (" DC_PRINTF_SIZE ").",
			dc_buffer_get_size (buffer));
		dc_buffer_free (buffer);
		return DC_STATUS_PROTOCOL;
	}

	// Get the number of dives.
	unsigned int ndives = array_uint16_le (dc_buffer_get_data(buffer));

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (1 + ndives * 2) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Download the dives.
	for (unsigned int i = 0; i < ndives; ++i) {
		// Erase the buffer.
		dc_buffer_clear (buffer);

		// Read the dive header.
		rc = mares_iconhd_read_object (device, &progress, buffer, OBJ_DIVE + i, OBJ_DIVE_HEADER);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			break;
		}

		// Check the fingerprint data.
		if (memcmp (dc_buffer_get_data (buffer) + 0x08, device->fingerprint, device->fingerprint_size) == 0) {
			INFO (abstract->context, "Stopping due to detecting a matching fingerprint");
			break;
		}

		// Read the dive data.
		rc = mares_iconhd_read_object (device, &progress, buffer, OBJ_DIVE + i, OBJ_DIVE_DATA);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive data.");
			break;
		}

		const unsigned char *data = dc_buffer_get_data (buffer);
		if (callback && !callback (data, dc_buffer_get_size (buffer), data + 0x08, device->fingerprint_size, userdata)) {
			break;
		}
	}

	dc_buffer_free(buffer);

	return rc;
}

static dc_status_t
mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	if (ISGENIUS(device->model)) {
		return mares_iconhd_device_foreach_object (abstract, callback, userdata);
	} else {
		return mares_iconhd_device_foreach_raw (abstract, callback, userdata);
	}
}
