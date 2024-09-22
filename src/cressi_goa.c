/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
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

#include <libdivecomputer/ble.h>

#include "cressi_goa.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &cressi_goa_device_vtable)

#define CMD_VERSION        0x00
#define CMD_SET_TIME       0x13
#define CMD_EXIT_PCLINK    0x1D
#define CMD_LOGBOOK        0x21
#define CMD_DIVE           0x22
#define CMD_LOGBOOK_V4     0x23

#define CMD_LOGBOOK_BLE 0x02
#define CMD_DIVE_BLE    0x03

#define HEADER  0xAA
#define TRAILER 0x55
#define END     0x04
#define ACK     0x06

#define SZ_DATA   512
#define SZ_PACKET 12

#define FP_SIZE   6

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct cressi_goa_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FP_SIZE];
} cressi_goa_device_t;

typedef struct cressi_goa_conf_t {
	unsigned int logbook_cmd;
	unsigned int logbook_len;
	unsigned int logbook_fp_offset;
	unsigned int dive_fp_offset;
} cressi_goa_conf_t;

static dc_status_t cressi_goa_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t cressi_goa_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t cressi_goa_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t cressi_goa_device_close (dc_device_t *abstract);

static const dc_device_vtable_t cressi_goa_device_vtable = {
	sizeof(cressi_goa_device_t),
	DC_FAMILY_CRESSI_GOA,
	cressi_goa_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	cressi_goa_device_foreach, /* foreach */
	cressi_goa_device_timesync, /* timesync */
	cressi_goa_device_close /* close */
};

static const cressi_goa_conf_t version_conf[] = {
	{ CMD_LOGBOOK,    23, 17, 12 },
	{ CMD_LOGBOOK_V4, 15,  3,  4 },
};

static dc_status_t
cressi_goa_device_send (cressi_goa_device_t *device, unsigned char cmd, const unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (size > SZ_PACKET) {
		ERROR (abstract->context, "Unexpected payload size (%u).", size);
		return DC_STATUS_INVALIDARGS;
	}

	// Setup the data packet.
	unsigned short crc = 0;
	unsigned char packet[SZ_PACKET + 8] = {
		HEADER, HEADER, HEADER,
		size,
		cmd
	};
	if (size) {
		memcpy (packet + 5, data, size);
	}
	crc = checksum_crc16_ccitt (packet + 3, size + 2, 0x000, 0x0000);
	packet[5 + size + 0] = (crc     ) & 0xFF; // Low
	packet[5 + size + 1] = (crc >> 8) & 0xFF; // High
	packet[5 + size + 2] = TRAILER;

	// Wait a small amount of time before sending the command. Without
	// this delay, the transfer will fail most of the time.
	unsigned int delay = transport == DC_TRANSPORT_BLE ? 2000 : 100;
	dc_iostream_sleep (device->iostream, delay);

	// Send the command to the device.
	if (transport == DC_TRANSPORT_BLE) {
		status = dc_iostream_write (device->iostream, packet + 4, size + 1, NULL);
	} else {
		status = dc_iostream_write (device->iostream, packet, size + 8, NULL);
	}
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return status;
}

static dc_status_t
cressi_goa_device_receive (cressi_goa_device_t *device, dc_buffer_t *output)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	unsigned char packet[SZ_PACKET + 8];

	if (transport == DC_TRANSPORT_BLE) {
		if (output) {
			return DC_STATUS_INVALIDARGS;
		} else {
			return DC_STATUS_SUCCESS;
		}
	}

	// Clear the output buffer.
	dc_buffer_clear (output);

	// Read the header of the data packet.
	status = dc_iostream_read (device->iostream, packet, 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header of the packet.
	if (packet[0] != HEADER || packet[1] != HEADER || packet[2] != HEADER) {
		ERROR (abstract->context, "Unexpected answer header byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Get the payload length.
	unsigned int length = packet[3];
	if (length > SZ_PACKET) {
		ERROR (abstract->context, "Unexpected payload size (%u).", length);
		return DC_STATUS_PROTOCOL;
	}

	// Read the remainder of the data packet.
	status = dc_iostream_read (device->iostream, packet + 4, length + 4, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the trailer of the packet.
	if (packet[length + 7] != TRAILER) {
		ERROR (abstract->context, "Unexpected answer trailer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Verify the checksum of the packet.
	unsigned short crc = array_uint16_le (packet + length + 5);
	unsigned short ccrc = checksum_crc16_ccitt (packet + 3, length + 2, 0x0000, 0x0000);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (length && output) {
		if (!dc_buffer_append (output, packet + 5, length)) {
			ERROR (abstract->context, "Could not append received data.");
			return DC_STATUS_NOMEMORY;
		}
	}

	return status;
}

static dc_status_t
cressi_goa_device_download (cressi_goa_device_t *device, dc_buffer_t *buffer, dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	const unsigned char ack[] = {ACK};
	const unsigned int initial = progress ? progress->current : 0;

	// Erase the contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned int skip = 2;
	unsigned int size = 2;
	unsigned int nbytes = 0;
	while (nbytes < size) {
		unsigned char packet[3 + SZ_DATA + 2];

		if (transport == DC_TRANSPORT_BLE) {
			// Read the data packet.
			unsigned int packetsize = 0;
			while (packetsize < SZ_DATA) {
				size_t len = 0;
				status = dc_iostream_read (device->iostream, packet + 3 + packetsize, SZ_DATA - packetsize, &len);
				if (status != DC_STATUS_SUCCESS) {
					ERROR (abstract->context, "Failed to receive the answer.");
					return status;
				}

				packetsize += len;
			}
		} else {
			// Read the data packet.
			status = dc_iostream_read (device->iostream, packet, sizeof(packet), NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to receive the answer.");
				return status;
			}

			// Verify the checksum of the packet.
			unsigned short crc = array_uint16_le (packet + sizeof(packet) - 2);
			unsigned short ccrc = checksum_crc16_ccitt (packet + 3, sizeof(packet) - 5, 0x0000, 0x0000);
			if (crc != ccrc) {
				ERROR (abstract->context, "Unexpected answer checksum.");
				return DC_STATUS_PROTOCOL;
			}

			// Send the ack byte to the device.
			status = dc_iostream_write (device->iostream, ack, sizeof(ack), NULL);
			if (status != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to send the ack byte.");
				return status;
			}
		}

		// Get the total size from the first data packet.
		if (nbytes == 0) {
			size += array_uint16_le (packet + 3);
		}

		// Calculate the payload size of the packet.
		unsigned int length = size - nbytes;
		if (length > SZ_DATA) {
			length = SZ_DATA;
		}

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, packet + 3 + skip, length - skip)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += length;
		skip = 0;

		// Update and emit a progress event.
		if (progress) {
			progress->current = initial + STEP(nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	if (transport == DC_TRANSPORT_BLE) {
		// Read the end bytes.
		unsigned char end[16] = {0};
		size_t len = 0;
		status = dc_iostream_read (device->iostream, end, sizeof(end), &len);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the end bytes.");
			return status;
		}

		// Verify the end bytes ("EOT xmodem").
		const unsigned char validate[16] = {
			0x45, 0x4F, 0x54, 0x20, 0x78, 0x6D, 0x6F, 0x64,
			0x65, 0x6D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		if (memcmp (end, validate, sizeof(validate)) != 0) {
			ERROR (abstract->context, "Unexpected end bytes.");
			return DC_STATUS_PROTOCOL;
		}
	} else {
		// Read the end byte.
		unsigned char end = 0;
		status = dc_iostream_read (device->iostream, &end, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the end byte.");
			return status;
		}

		// Verify the end byte.
		if (end != END) {
			ERROR (abstract->context, "Unexpected end byte (%02x).", end);
			return DC_STATUS_PROTOCOL;
		}

		// Send the ack byte to the device.
		status = dc_iostream_write (device->iostream, ack, sizeof(ack), NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the ack byte.");
			return status;
		}
	}

	return status;
}

static dc_status_t
cressi_goa_device_transfer (cressi_goa_device_t *device,
                            unsigned char cmd,
                            const unsigned char input[], unsigned int isize,
                            dc_buffer_t *output,
                            dc_buffer_t *buffer,
                            dc_event_progress_t *progress)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	// Send the command to the dive computer.
	status = cressi_goa_device_send (device, cmd, input, isize);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Receive the answer from the dive computer.
	status = cressi_goa_device_receive (device, output);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Download the optional and variable sized payload.
	if (buffer) {
		status = cressi_goa_device_download (device, buffer, progress);
		if (status != DC_STATUS_SUCCESS) {
			return status;
		}
	}

	return status;
}


dc_status_t
cressi_goa_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (cressi_goa_device_t *) dc_device_allocate (context, &cressi_goa_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000 - 5000 ms).
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);
	int timeout = transport == DC_TRANSPORT_BLE ? 5000 : 3000;
	status = dc_iostream_set_timeout (device->iostream, timeout);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Clear the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_free;
	}

	dc_iostream_sleep (device->iostream, 100);
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
cressi_goa_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
cressi_goa_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);
	dc_buffer_t *logbook = NULL;
	dc_buffer_t *dive = NULL;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *id = dc_buffer_new(11);
	if (id == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for the ID.");
		status = DC_STATUS_NOMEMORY;
		goto error_exit;
	}

	// Read the version information.
	if (transport == DC_TRANSPORT_BLE) {
		/*
		 * With the BLE communication, there is no variant of the CMD_VERSION
		 * command available. The corresponding information must be obtained by
		 * reading some secondary characteristics instead:
		 *     6E400003-B5A3-F393-E0A9-E50E24DC10B8 - 5 bytes
		 *     6E400004-B5A3-F393-E0A9-E50E24DC10B8 - 2 bytes
		 *     6E400005-B5A3-F393-E0A9-E50E24DC10B8 - 2 bytes
		 */
		const dc_ble_uuid_t characteristics[] = {
			{0x6E, 0x40, 0x00, 0x03, 0xB5, 0xA3, 0xF3, 0x93, 0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0x10, 0xB8},
			{0x6E, 0x40, 0x00, 0x04, 0xB5, 0xA3, 0xF3, 0x93, 0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0x10, 0xB8},
			{0x6E, 0x40, 0x00, 0x05, 0xB5, 0xA3, 0xF3, 0x93, 0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0x10, 0xB8},
		};
		const size_t sizes[] = {5, 2, 2};

		for (size_t i = 0; i < C_ARRAY_SIZE(characteristics); ++i) {
			unsigned char request[sizeof(dc_ble_uuid_t) + 5] = {0};

			// Setup the request.
			memcpy (request, characteristics[i], sizeof(dc_ble_uuid_t));
			memset (request + sizeof(dc_ble_uuid_t), 0, sizes[i]);

			// Read the characteristic.
			status = dc_iostream_ioctl (device->iostream, DC_IOCTL_BLE_CHARACTERISTIC_READ, request, sizeof(dc_ble_uuid_t) + sizes[i]);
			if (status != DC_STATUS_SUCCESS) {
				char uuidstr[DC_BLE_UUID_SIZE] = {0};
				ERROR (abstract->context, "Failed to read the characteristic '%s'.",
					dc_ble_uuid2str(characteristics[i], uuidstr, sizeof(uuidstr)));
				goto error_free_id;
			}

			// Copy the payload data.
			if (!dc_buffer_append(id, request + sizeof(dc_ble_uuid_t), sizes[i])) {
				ERROR (abstract->context, "Insufficient buffer space available.");
				status = DC_STATUS_NOMEMORY;
				goto error_free_id;
			}
		}
	} else {
		status = cressi_goa_device_transfer (device, CMD_VERSION, NULL, 0, id, NULL, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the version information.");
			goto error_free_id;
		}
	}

	const unsigned char *id_data = dc_buffer_get_data(id);
	size_t id_size = dc_buffer_get_size(id);

	if (id_size < 9) {
		ERROR (abstract->context, "Unexpected version length (" DC_PRINTF_SIZE ").", id_size);
		status = DC_STATUS_DATAFORMAT;
		goto error_free_id;
	}

	// Get the device info.
	unsigned int model = id_data[4];
	unsigned int firmware = array_uint16_le (id_data + 5);
	unsigned int serial = array_uint32_le (id_data + 0);

	// Get the data format version.
	unsigned int version = 0;
	if (id_size == 11) {
		version = array_uint16_le (id_data + 9);
	} else {
		if (firmware >= 161 && firmware <= 165) {
			version = 0;
		} else if (firmware >= 166 && firmware <= 169) {
			version = 1;
		} else if (firmware >= 170 && firmware <= 179) {
			version = 2;
		} else if (firmware >= 100 && firmware <= 110) {
			version = 3;
		} else if (firmware >= 200 && firmware <= 205) {
			version = 4;
		} else if (firmware >= 300) {
			version = 5;
		} else {
			ERROR (abstract->context, "Unknown firmware version (%u).", firmware);
			status = DC_STATUS_DATAFORMAT;
			goto error_free_id;
		}
	}

	const cressi_goa_conf_t *conf = &version_conf[version >= 4];

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = id_data;
	vendor.size = id_size;
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model;
	devinfo.firmware = firmware;
	devinfo.serial = serial;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Allocate memory for the logbook data.
	logbook = dc_buffer_new(4096);
	if (logbook == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for the logbook.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_id;
	}

	// Read the logbook data.
	if (transport == DC_TRANSPORT_BLE) {
		unsigned char args[] = {0x00};
		status = cressi_goa_device_transfer (device, CMD_LOGBOOK_BLE, args, sizeof(args), NULL, logbook, &progress);
	} else {
		status = cressi_goa_device_transfer (device, conf->logbook_cmd, NULL, 0, NULL, logbook, &progress);
	}
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the logbook data.");
		goto error_free_logbook;
	}

	const unsigned char *logbook_data = dc_buffer_get_data(logbook);
	size_t logbook_size = dc_buffer_get_size(logbook);

	// Count the number of dives.
	unsigned int count = 0;
	unsigned int offset = logbook_size;
	while (offset >= conf->logbook_len) {
		// Move to the start of the logbook entry.
		offset -= conf->logbook_len;

		// Get the dive number.
		unsigned int number= array_uint16_le (logbook_data + offset);
		if (number == 0)
			break;

		// Compare the fingerprint to identify previously downloaded entries.
		if (memcmp (logbook_data + offset + conf->logbook_fp_offset, device->fingerprint, sizeof(device->fingerprint)) == 0) {
			break;
		}

		count++;
	}

	// Update and emit a progress event.
	progress.maximum = (count + 1) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the dive data.
	dive = dc_buffer_new(4096);
	if (dive == NULL) {
		ERROR (abstract->context, "Failed to allocate memory for the dive.");
		status = DC_STATUS_NOMEMORY;
		goto error_free_logbook;
	}

	// Download the dives.
	offset = logbook_size;
	for (unsigned int i = 0; i < count; ++i) {
		// Move to the start of the logbook entry.
		offset -= conf->logbook_len;

		// Read the dive data.
		if (transport == DC_TRANSPORT_BLE) {
			unsigned char number[2] = {
				logbook_data[offset + 1],
				logbook_data[offset + 0]};
			status = cressi_goa_device_transfer (device, CMD_DIVE_BLE, number, 2, NULL, dive, &progress);
		} else {
			status = cressi_goa_device_transfer (device, CMD_DIVE, logbook_data + offset, 2, NULL, dive, &progress);
		}
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive data.");
			goto error_free_dive;
		}

		const unsigned char *dive_data = dc_buffer_get_data (dive);
		size_t dive_size = dc_buffer_get_size (dive);

		// Verify the dive number and the fingerprint in the logbook and dive
		// data are identical.
		if (dive_size < conf->dive_fp_offset + FP_SIZE ||
			memcmp (dive_data, logbook_data + offset, 2) != 0 ||
			memcmp (dive_data + conf->dive_fp_offset, logbook_data + offset + conf->logbook_fp_offset, FP_SIZE) != 0) {
			ERROR (abstract->context, "Unexpected dive header.");
			status = DC_STATUS_DATAFORMAT;
			goto error_free_dive;
		}

		// The dive computer id data and the logbook entry contain important
		// information that is required for parsing the dive data, but is
		// not present in the dive data itself. Therefore, both pieces of data
		// are prepended to the dive data, along with a small header containing
		// their size.
		const unsigned char header[] = {
			id_size,
			conf->logbook_len,
		};
		unsigned int headersize = sizeof(header) + id_size + conf->logbook_len;
		if (!dc_buffer_prepend(dive, logbook_data + offset, conf->logbook_len) ||
			!dc_buffer_prepend(dive, id_data, id_size) ||
			!dc_buffer_prepend(dive, header, sizeof(header))) {
			ERROR (abstract->context, "Out of memory.");
			status = DC_STATUS_NOMEMORY;
			goto error_free_dive;
		}

		dive_data = dc_buffer_get_data (dive);
		dive_size = dc_buffer_get_size (dive);

		if (callback && !callback(dive_data, dive_size, dive_data + headersize + conf->dive_fp_offset, sizeof(device->fingerprint), userdata))
			break;
	}

error_free_dive:
	dc_buffer_free(dive);
error_free_logbook:
	dc_buffer_free(logbook);
error_free_id:
	dc_buffer_free(id);
error_exit:
	return status;
}

static dc_status_t
cressi_goa_device_timesync (dc_device_t *abstract, const dc_datetime_t *datetime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (transport == DC_TRANSPORT_BLE) {
		return DC_STATUS_UNSUPPORTED;
	}

	unsigned char new_time[7];
	array_uint16_le_set(new_time, datetime->year);
	new_time[2] = datetime->month;
	new_time[3] = datetime->day;
	new_time[4] = datetime->hour;
	new_time[5] = datetime->minute;
	new_time[6] = datetime->second;
	status = cressi_goa_device_transfer (device, CMD_SET_TIME, new_time, sizeof(new_time), NULL, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to set the new time.");
		return status;
	}

	return status;
}

static dc_status_t
cressi_goa_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cressi_goa_device_t *device = (cressi_goa_device_t *) abstract;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (transport == DC_TRANSPORT_BLE) {
		return DC_STATUS_SUCCESS;
	}

	status = cressi_goa_device_transfer (device, CMD_EXIT_PCLINK, NULL, 0, NULL, NULL, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to exit PC Link.");
		return status;
	}

	return status;
}
