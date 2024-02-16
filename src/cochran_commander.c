/*
 * libdivecomputer
 *
 * Copyright (C) 2014 John Van Ostrand
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

#include "cochran_commander.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "ringbuffer.h"
#include "rbstream.h"

#define MAXRETRIES 2

#define COCHRAN_MODEL_COMMANDER_TM 0
#define COCHRAN_MODEL_COMMANDER_PRE21000 1
#define COCHRAN_MODEL_COMMANDER_AIR_NITROX 2
#define COCHRAN_MODEL_EMC_14 3
#define COCHRAN_MODEL_EMC_16 4
#define COCHRAN_MODEL_EMC_20 5

#define UNDEFINED 0xFFFFFFFF

typedef enum cochran_endian_t {
	ENDIAN_LE,
	ENDIAN_BE,
	ENDIAN_WORD_BE,
} cochran_endian_t;

typedef struct cochran_commander_model_t {
	unsigned char id[3 + 1];
	unsigned int model;
} cochran_commander_model_t;

typedef struct cochran_data_t {
	unsigned char config[1024];
	unsigned char *logbook;

	unsigned short int dive_count;
	unsigned int fp_dive_num;
	unsigned int invalid_profile_dive_num;

	unsigned int logbook_size;
} cochran_data_t;

typedef struct cochran_device_layout_t {
	unsigned int model;
	unsigned int address_bits;
	cochran_endian_t endian;
	unsigned int baudrate;
	unsigned int rbstream_size;
	// Config data.
	unsigned int cf_dive_count;
	unsigned int cf_last_log;
	unsigned int cf_last_interdive;
	unsigned int cf_serial_number;
	// Logbook ringbuffer.
	unsigned int rb_logbook_begin;
	unsigned int rb_logbook_end;
	unsigned int rb_logbook_entry_size;
	unsigned int rb_logbook_entry_count;
	// Profile ringbuffer.
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	// pointers.
	unsigned int pt_fingerprint;
	unsigned int fingerprint_size;
	unsigned int pt_profile_pre;
	unsigned int pt_profile_begin;
	unsigned int pt_profile_end;
	unsigned int pt_dive_number;
} cochran_device_layout_t;

typedef struct cochran_commander_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	const cochran_device_layout_t *layout;
	unsigned char id[67];
	unsigned char fingerprint[6];
} cochran_commander_device_t;

static dc_status_t cochran_commander_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t cochran_commander_device_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t cochran_commander_device_dump (dc_device_t *device, dc_buffer_t *data);
static dc_status_t cochran_commander_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t cochran_commander_device_vtable = {
	sizeof (cochran_commander_device_t),
	DC_FAMILY_COCHRAN_COMMANDER,
	cochran_commander_device_set_fingerprint,/* set_fingerprint */
	cochran_commander_device_read, /* read */
	NULL, /* write */
	cochran_commander_device_dump, /* dump */
	cochran_commander_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};

// Cochran Commander TM, pre-dates pre-21000 s/n
static const cochran_device_layout_t cochran_cmdr_tm_device_layout = {
	COCHRAN_MODEL_COMMANDER_TM, // model
	24,         // address_bits
	ENDIAN_WORD_BE,	// endian
	9600,       // baudrate
	4096,       // rbstream_size
	0x146,      // cf_dive_count
	0x158,      // cf_last_log
	0xffffff,   // cf_last_interdive
	0x15c,      // cf_serial_number
	0x010000,   // rb_logbook_begin
	0x01232b,   // rb_logbook_end
	90,         // rb_logbook_entry_size
	100,        // rb_logbook_entry_count
	0x01232b,   // rb_profile_begin
	0x018000,   // rb_profile_end
	15,         // pt_fingerprint
	4,          // fingerprint_size
	0,          // pt_profile_pre
	0,          // pt_profile_begin
	90,         // pt_profile_end (Next begin pointer is the end)
	20,         // pt_dive_number
};

// Cochran Commander pre-21000 s/n
static const cochran_device_layout_t cochran_cmdr_1_device_layout = {
	COCHRAN_MODEL_COMMANDER_PRE21000, // model
	24,         // address_bits
	ENDIAN_WORD_BE,  // endian
	115200,     // baudrate
	32768,      // rbstream_size
	0x046,      // cf_dive_count
	0x6c,       // cf_last_log
	0x70,       // cf_last_interdive
	0x0AA,      // cf_serial_number
	0x00000000, // rb_logbook_begin
	0x00020000, // rb_logbook_end
	256,        // rb_logbook_entry_size
	512,        // rb_logbook_entry_count
	0x00020000, // rb_profile_begin
	0x00100000, // rb_profile_end
	12,         // pt_fingerprint
	4,          // fingerprint_size
	28,         // pt_profile_pre
	0,          // pt_profile_begin
	128,        // pt_profile_end
	68,         // pt_dive_number
};


// Cochran Commander Nitrox
static const cochran_device_layout_t cochran_cmdr_device_layout = {
	COCHRAN_MODEL_COMMANDER_AIR_NITROX, // model
	24,         // address_bits
	ENDIAN_WORD_BE,  // endian
	115200,     // baudrate
	32768,      // rbstream_size
	0x046,      // cf_dive_count
	0x06C,      // cf_last_log
	0x070,      // cf_last_interdive
	0x0AA,      // cf_serial_number
	0x00000000, // rb_logbook_begin
	0x00020000, // rb_logbook_end
	256,        // rb_logbook_entry_size
	512,        // rb_logbook_entry_count
	0x00020000, // rb_profile_begin
	0x00100000, // rb_profile_end
	0,          // pt_fingerprint
	6,          // fingerprint_size
	30,         // pt_profile_pre
	6,          // pt_profile_begin
	128,        // pt_profile_end
	70,         // pt_dive_number
};

// Cochran EMC-14
static const cochran_device_layout_t cochran_emc14_device_layout = {
	COCHRAN_MODEL_EMC_14, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	850000,     // baudrate
	32768,      // rbstream_size
	0x0D2,      // cf_dive_count
	0x13E,      // cf_last_log
	0x142,      // cf_last_interdive
	0x1E6,      // cf_serial_number
	0x00000000, // rb_logbook_begin
	0x00020000, // rb_logbook_end
	512,        // rb_logbook_entry_size
	256,        // rb_logbook_entry_count
	0x00022000, // rb_profile_begin
	0x00200000, // rb_profile_end
	0,          // pt_fingerprint
	6,          // fingerprint_size
	30,         // pt_profile_pre
	6,          // pt_profile_begin
	256,        // pt_profile_end
	86,         // pt_dive_number
};

// Cochran EMC-16
static const cochran_device_layout_t cochran_emc16_device_layout = {
	COCHRAN_MODEL_EMC_16, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	850000,     // baudrate
	32768,      // rbstream_size
	0x0D2,      // cf_dive_count
	0x13E,      // cf_last_log
	0x142,      // cf_last_interdive
	0x1E6,      // cf_serial_number
	0x00000000, // rb_logbook_begin
	0x00080000, // rb_logbook_end
	512,        // rb_logbook_entry_size
	1024,       // rb_logbook_entry_count
	0x00094000, // rb_profile_begin
	0x00800000, // rb_profile_end
	0,          // pt_fingerprint
	6,          // fingerprint_size
	30,         // pt_profile_pre
	6,          // pt_profile_begin
	256,        // pt_profile_end
	86,         // pt_dive_number
};

// Cochran EMC-20
static const cochran_device_layout_t cochran_emc20_device_layout = {
	COCHRAN_MODEL_EMC_20, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	850000,     // baudrate
	32768,      // rbstream_size
	0x0D2,      // cf_dive_count
	0x13E,      // cf_last_log
	0x142,      // cf_last_interdive
	0x1E6,      // cf_serial_number
	0x00000000, // rb_logbook_begin
	0x00080000, // rb_logbook_end
	512,        // rb_logbook_entry_size
	1024,       // rb_logbook_entry_count
	0x00094000, // rb_profile_begin
	0x01000000, // rb_profile_end
	0,          // pt_fingerprint
	6,          // fingerprint_size
	30,         // pt_profile_pre
	6,          // pt_profile_begin
	256,        // pt_profile_end
	86,         // pt_dive_number
};


// Determine model descriptor number from model string
static unsigned int
cochran_commander_get_model (cochran_commander_device_t *device)
{
	const cochran_commander_model_t models[] = {
		{"\x0a""12", COCHRAN_MODEL_COMMANDER_TM},
		{"\x11""21", COCHRAN_MODEL_COMMANDER_PRE21000},
		{"\x11""22", COCHRAN_MODEL_COMMANDER_AIR_NITROX},
		{"730",      COCHRAN_MODEL_EMC_14},
		{"731",      COCHRAN_MODEL_EMC_14},
		{"A30",      COCHRAN_MODEL_EMC_16},
		{"A31",      COCHRAN_MODEL_EMC_16},
		{"230",      COCHRAN_MODEL_EMC_20},
		{"231",      COCHRAN_MODEL_EMC_20},
		{"\x40""30", COCHRAN_MODEL_EMC_20},
	};

	unsigned int model = 0xFFFFFFFF;
	for (unsigned int i = 0; i < C_ARRAY_SIZE(models); ++i) {
		if (memcmp (device->id + 0x3D, models[i].id, sizeof(models[i].id) - 1) == 0) {
			model = models[i].model;
			break;
		}
	}

	return model;
}


static dc_status_t
cochran_commander_serial_setup (cochran_commander_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	// Set the serial communication protocol (9600 8N2, no FC).
	status = dc_iostream_configure (device->iostream, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to set the terminal attributes.");
		return status;
	}

	// Set the timeout for receiving data (5000 ms).
	status = dc_iostream_set_timeout (device->iostream, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to set the timeout.");
		return status;
	}

	// Wake up DC and trigger heartbeat
	dc_iostream_set_break(device->iostream, 1);
	dc_iostream_sleep(device->iostream, 16);
	dc_iostream_set_break(device->iostream, 0);

	// Clear old heartbeats
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Wait for heartbeat byte before send
	unsigned char answer = 0;
	status = dc_iostream_read(device->iostream, &answer, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to receive device heartbeat.");
		return status;
	}

	if (answer != 0xAA) {
		ERROR (device->base.context, "Received bad hearbeat byte (%02x).", answer);
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_packet (cochran_commander_device_t *device, dc_event_progress_t *progress,
	const unsigned char command[], unsigned int csize,
	unsigned char answer[], unsigned int asize, int high_speed)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t status = DC_STATUS_SUCCESS;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Send the command to the device, one byte at a time
	// If sent all at once the command is ignored. It's like the DC
	// has no buffering.
	for (unsigned int i = 0; i < csize; i++) {
		// Give the DC time to read the character.
		if (i) dc_iostream_sleep(device->iostream, 16); // 16 ms

		status = dc_iostream_write(device->iostream, command + i, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}
	}

	if (high_speed && device->layout->baudrate != 9600) {
		// Give the DC time to process the command.
		dc_iostream_sleep(device->iostream, 45);

		// Rates are odd, like 850400 for the EMC, 115200 for commander
		status = dc_iostream_configure(device->iostream, device->layout->baudrate, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to set the high baud rate.");
			return status;
		}
	}

	// Receive the answer from the device.
	// Use 1024 byte "packets" so we can display progress.
	unsigned int nbytes = 0;
	while (nbytes < asize) {
		unsigned int len = asize - nbytes;
		if (len > 1024)
			len = 1024;

		status = dc_iostream_read (device->iostream, answer + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive data.");
			return status;
		}

		nbytes += len;

		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_read_id (cochran_commander_device_t *device, unsigned char id[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	unsigned char command[6] = {0x05, 0x9D, 0xFF, 0x00, 0x43, 0x00};

	rc = cochran_commander_packet(device, NULL, command, sizeof(command), id, size, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	if (memcmp(id, "(C)", 3) != 0) {
		// It's a Commander, read a different location
		command[1] = 0xBD;
		command[2] = 0x7F;

		rc = cochran_commander_packet(device, NULL, command, sizeof(command), id, size, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_read_config (cochran_commander_device_t *device, dc_event_progress_t *progress, unsigned char data[], unsigned int size)
{
	dc_device_t *abstract = (dc_device_t *) device;
	dc_status_t rc = DC_STATUS_SUCCESS;

	if ((size % 512) != 0)
		return DC_STATUS_INVALIDARGS;

	// Read two 512 byte blocks into one 1024 byte buffer
	unsigned int pages = size / 512;
	for (unsigned int i = 0; i < pages; i++) {
		unsigned char command[2] = {0x96, i};
		unsigned int command_size = sizeof(command);
		if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
			command_size = 1;

		rc = cochran_commander_packet(device, progress, command, command_size, data + i * 512, 512, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		dc_event_vendor_t vendor;
		vendor.data = data + i * 512;
		vendor.size = 512;
		device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_read (cochran_commander_device_t *device, dc_event_progress_t *progress, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Build the command
	unsigned char command[10];
	unsigned char command_size;

	switch (device->layout->address_bits) {
	case 32:
		// EMC uses 32 bit addressing
		command[0] = 0x15;
		command[1] = (address      ) & 0xff;
		command[2] = (address >>  8) & 0xff;
		command[3] = (address >> 16) & 0xff;
		command[4] = (address >> 24) & 0xff;
		command[5] = (size         ) & 0xff;
		command[6] = (size >>  8   ) & 0xff;
		command[7] = (size >> 16   ) & 0xff;
		command[8] = (size >> 24   ) & 0xff;
		command[9] = 0x05;
		command_size = 10;
		break;
	case 24:
		// Commander uses 24 byte addressing
		if (device->layout->baudrate == 9600) {
			// This read command will return 32K bytes if asked to read
			// 0 bytes. So we can allow a size of up to 0x10000 but if
			// the user asks for 0 bytes we should just return success
			// otherwise we'll end end up running past the buffer.
			if (size > 0x10000)
				return DC_STATUS_INVALIDARGS;
			if (size == 0)
				return DC_STATUS_SUCCESS;

			// Older commander, use low-speed read command
			command[0] = 0x05;
			command[1] = (address      ) & 0xff;
			command[2] = (address >>  8) & 0xff;
			command[3] = (address >> 16) & 0xff;
			command[4] = (size         ) & 0xff;
			command[5] = (size >> 8    ) & 0xff;
			command_size = 6;
		} else {
			// Newer commander with high-speed read command
			command[0] = 0x15;
			command[1] = (address      ) & 0xff;
			command[2] = (address >>  8) & 0xff;
			command[3] = (address >> 16) & 0xff;
			command[4] = (size         ) & 0xff;
			command[5] = (size >>  8   ) & 0xff;
			command[6] = (size >> 16   ) & 0xff;
			command[7] = 0x04;
			command_size = 8;
		}
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}

	dc_iostream_sleep(device->iostream, 550);

	// set back to 9600 baud
	rc = cochran_commander_serial_setup(device);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read data at high speed
	rc = cochran_commander_packet (device, progress, command, command_size, data, size, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_read_retry (cochran_commander_device_t *device, dc_event_progress_t *progress, unsigned int address, unsigned char data[], unsigned int size)
{
	// Save the state of the progress events.
	unsigned int saved = 0;
	if (progress) {
		saved = progress->current;
	}

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = cochran_commander_read (device, progress, address, data, size)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Restore the state of the progress events.
		if (progress) {
			progress->current = saved;
		}
	}

	return rc;
}


/*
 *  For corrupt dives the end-of-samples pointer is 0xFFFFFFFF
 *  search for a reasonable size, e.g. using next dive start sample
 *  or end-of-samples to limit searching for recoverable samples
 */
static unsigned int
cochran_commander_guess_sample_end_address(cochran_commander_device_t *device, cochran_data_t *data, unsigned int log_num)
{
	const unsigned char *log_entry = data->logbook + device->layout->rb_logbook_entry_size * log_num;

	if (log_num == data->dive_count)
		// Return next usable address from config page
		return array_uint32_le(data->config + device->layout->rb_profile_end);

	// Next log's start address
	return array_uint32_le(log_entry + device->layout->rb_logbook_entry_size + device->layout->pt_profile_begin);
}


static unsigned int
cochran_commander_profile_size(cochran_commander_device_t *device, cochran_data_t *data, int dive_num, unsigned int sample_start_address, unsigned int sample_end_address)
{
	// Validate addresses
	if (sample_start_address < device->layout->rb_profile_begin ||
		sample_start_address > device->layout->rb_profile_end ||
		sample_end_address < device->layout->rb_profile_begin ||
		(sample_end_address > device->layout->rb_profile_end &&
		sample_end_address != 0xFFFFFFFF)) {
		return 0;
	}

	if (sample_end_address == 0xFFFFFFFF)
		// Corrupt dive, guess the end address
		sample_end_address = cochran_commander_guess_sample_end_address(device, data, dive_num);

	return ringbuffer_distance(sample_start_address, sample_end_address, DC_RINGBUFFER_EMPTY, device->layout->rb_profile_begin, device->layout->rb_profile_end);
}


/*
 * Do several things. Find the log that matches the fingerprint,
 * calculate the total read size for progress indicator,
 * Determine the most recent dive without profile data.
 */

static unsigned int
cochran_commander_find_fingerprint(cochran_commander_device_t *device, cochran_data_t *data)
{
	unsigned int base = device->layout->rb_logbook_begin;

	// We track profile ringbuffer usage to determine which dives have profile data
	int profile_capacity_remaining = device->layout->rb_profile_end - device->layout->rb_profile_begin;

	unsigned int dive_count = 0;
	data->fp_dive_num = UNDEFINED;

	// Start at end of log
	if (data->dive_count < device->layout->rb_logbook_entry_count)
		dive_count = data->dive_count;
	else
		dive_count = device->layout->rb_logbook_entry_count;

	unsigned int sample_read_size = 0;
	data->invalid_profile_dive_num = UNDEFINED;

	// Remove the pre-dive events that occur after the last dive
	unsigned int rb_head_ptr = 0;
	if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
		// TM uses SRAM and does not need to erase pages
		rb_head_ptr = base + array_uint16_be(data->config + device->layout->cf_last_log);
	else if (device->layout->endian == ENDIAN_WORD_BE)
		rb_head_ptr = base + (array_uint32_word_be(data->config + device->layout->cf_last_log) & 0xfffff000) + 0x2000;
	else
		rb_head_ptr = base + (array_uint32_le(data->config + device->layout->cf_last_log) & 0xfffff000) + 0x2000;

	unsigned int head_dive = 0, tail_dive = 0;

	if (data->dive_count <= device->layout->rb_logbook_entry_count) {
		head_dive = data->dive_count;
		tail_dive = 0;
	} else {
		// Log wrapped
		tail_dive = data->dive_count % device->layout->rb_logbook_entry_count;
		head_dive = tail_dive;
	}

	unsigned int last_profile_idx = (device->layout->rb_logbook_entry_count + head_dive - 1) % device->layout->rb_logbook_entry_count;
	unsigned int last_profile_end = 0;
	if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
		// There is no end pointer in this model and no inter-dive
		// events. We could use profile_begin from the next dive but
		// since this is the last dive, we'll use rb_head_ptr
		last_profile_end = rb_head_ptr;
	else
		last_profile_end = base + array_uint32_le(data->logbook + last_profile_idx * device->layout->rb_logbook_entry_size + device->layout->pt_profile_end);

	unsigned int last_profile_pre = 0xFFFFFFFF;
	if (device->layout->endian == ENDIAN_WORD_BE)
		last_profile_pre = base + array_uint32_word_be(data->config + device->layout->cf_last_log);
	else
		last_profile_pre = base + array_uint32_le(data->config + device->layout->cf_last_log);

	if (rb_head_ptr > last_profile_end)
		profile_capacity_remaining -= rb_head_ptr - last_profile_end;

	// Loop through dives to find FP, Accumulate profile data size,
	// and find the last dive with invalid profile
	for (unsigned int i = 0; i < dive_count; ++i) {
		unsigned int idx = (device->layout->rb_logbook_entry_count + head_dive - (i + 1)) % device->layout->rb_logbook_entry_count;

		unsigned char *log_entry = data->logbook + idx * device->layout->rb_logbook_entry_size;

		// We're done if we find the fingerprint
		if (!memcmp(device->fingerprint, log_entry + device->layout->pt_fingerprint, device->layout->fingerprint_size)) {
			data->fp_dive_num = idx;
			break;
		}

		unsigned int profile_pre = 0;
		if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
			profile_pre = base + array_uint16_le(log_entry + device->layout->pt_profile_pre);
		else
			profile_pre = base + array_uint32_le(log_entry + device->layout->pt_profile_pre);

		unsigned int sample_size = cochran_commander_profile_size(device, data, idx, profile_pre, last_profile_pre);
		last_profile_pre = profile_pre;

		// Determine if sample exists
		if (profile_capacity_remaining > 0) {
			// Subtract this dive's profile size including post-dive events
			profile_capacity_remaining -= sample_size;
			if (profile_capacity_remaining < 0) {
				// Save the last dive that is missing profile data
				data->invalid_profile_dive_num = idx;
			}
			// Accumulate read size for progress bar
			sample_read_size += sample_size;
		}
	}

	return sample_read_size;
}


dc_status_t
cochran_commander_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cochran_commander_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (cochran_commander_device_t *) dc_device_allocate (context, &cochran_commander_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	cochran_commander_device_set_fingerprint((dc_device_t *) device, NULL, 0);

	status = cochran_commander_serial_setup(device);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	// Read ID from the device
	status = cochran_commander_read_id (device, device->id, sizeof(device->id));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Device not responding.");
		goto error_free;
	}

	unsigned int model = cochran_commander_get_model(device);
	switch (model) {
	case COCHRAN_MODEL_COMMANDER_TM:
		device->layout = &cochran_cmdr_tm_device_layout;
		break;
	case COCHRAN_MODEL_COMMANDER_PRE21000:
		device->layout = &cochran_cmdr_1_device_layout;
		break;
	case COCHRAN_MODEL_COMMANDER_AIR_NITROX:
		device->layout = &cochran_cmdr_device_layout;
		break;
	case COCHRAN_MODEL_EMC_14:
		device->layout = &cochran_emc14_device_layout;
		break;
	case COCHRAN_MODEL_EMC_16:
		device->layout = &cochran_emc16_device_layout;
		break;
	case COCHRAN_MODEL_EMC_20:
		device->layout = &cochran_emc20_device_layout;
		break;
	default:
		ERROR (context, "Unknown model");
		status = DC_STATUS_UNSUPPORTED;
		goto error_free;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
cochran_commander_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;

	if (size && size != device->layout->fingerprint_size)
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, device->layout->fingerprint_size);
	else
		memset (device->fingerprint, 0xFF, sizeof(device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;

	return cochran_commander_read_retry(device, NULL, address, data, size);
}


static dc_status_t
cochran_commander_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char config[1024];
	unsigned int config_size = sizeof(config);
	unsigned int size = device->layout->rb_profile_end - device->layout->rb_logbook_begin;

	// Reserve space
	if (!dc_buffer_resize(buffer, size)) {
		ERROR(abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
		config_size = 512;

	// Determine size for progress
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = config_size + size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit ID block
	dc_event_vendor_t vendor;
	vendor.data = device->id;
	vendor.size = sizeof (device->id);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	rc = cochran_commander_read_config (device, &progress, config, config_size);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the sample data, logbook and sample data are contiguous
	rc = cochran_commander_read_retry (device, &progress, device->layout->rb_logbook_begin, dc_buffer_get_data(buffer), size);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the sample data.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;
	const cochran_device_layout_t *layout = device->layout;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_rbstream_t *rbstream = NULL;

	cochran_data_t data;
	data.logbook = NULL;

	// Calculate max data sizes
	unsigned int max_config = sizeof(data.config);
	unsigned int max_logbook = layout->rb_logbook_end - layout->rb_logbook_begin;
	unsigned int max_sample = layout->rb_profile_end - layout->rb_profile_begin;
	unsigned int base = device->layout->rb_logbook_begin;

	if (device->layout->model == COCHRAN_MODEL_COMMANDER_TM)
		max_config = 512;

	// setup progress indication
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = max_config + max_logbook + max_sample;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit ID block
	dc_event_vendor_t vendor;
	vendor.data = device->id;
	vendor.size = sizeof (device->id);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read config
	dc_status_t rc = DC_STATUS_SUCCESS;
	rc = cochran_commander_read_config(device, &progress, data.config, max_config);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Determine size of dive list to read.
	if (layout->endian == ENDIAN_LE)
		data.dive_count = array_uint16_le (data.config + layout->cf_dive_count);
	else
		data.dive_count = array_uint16_be (data.config + layout->cf_dive_count);

	if (data.dive_count == 0) {
		// No dives to read
		WARNING(abstract->context, "This dive computer has no recorded dives.");
		return DC_STATUS_SUCCESS;
	}

	if (data.dive_count > layout->rb_logbook_entry_count) {
		data.logbook_size = layout->rb_logbook_entry_count * layout->rb_logbook_entry_size;
	} else {
		data.logbook_size = data.dive_count * layout->rb_logbook_entry_size;
	}

	// Update progress indicator with new maximum
	progress.maximum -= max_logbook - data.logbook_size;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate space for log book.
	data.logbook = (unsigned char *) malloc(data.logbook_size);
	if (data.logbook == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Request log book
	rc = cochran_commander_read_retry(device, &progress, layout->rb_logbook_begin, data.logbook, data.logbook_size);
	if (rc != DC_STATUS_SUCCESS) {
		status = rc;
		goto error;
	}

	// Locate fingerprint, recent dive with invalid profile and calc read size
	unsigned int profile_read_size = cochran_commander_find_fingerprint(device, &data);

	// Update progress indicator with new maximum
	progress.maximum -= (max_sample - profile_read_size);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = layout->model;
	devinfo.firmware = 0; // unknown
	if (layout->endian == ENDIAN_WORD_BE)
		devinfo.serial = array_uint32_word_be(data.config + layout->cf_serial_number);
	else
		devinfo.serial = array_uint32_le(data.config + layout->cf_serial_number);

	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	unsigned int head_dive = 0, tail_dive = 0, dive_count = 0;

	if (data.dive_count <= layout->rb_logbook_entry_count) {
		head_dive = data.dive_count;
		tail_dive = 0;
	} else {
		// Log wrapped
		tail_dive = data.dive_count % layout->rb_logbook_entry_count;
		head_dive = tail_dive;
	}

	// Change tail to dive following the fingerprint dive.
	if (data.fp_dive_num != UNDEFINED)
		tail_dive = (data.fp_dive_num + 1) % layout->rb_logbook_entry_count;

	// Number of dives to read
	dive_count = (layout->rb_logbook_entry_count + head_dive - tail_dive) % layout->rb_logbook_entry_count;

	unsigned int last_start_address = 0;
	if (layout->endian == ENDIAN_WORD_BE)
		last_start_address = base + array_uint32_word_be(data.config + layout->cf_last_log );
	else
		last_start_address = base + array_uint32_le(data.config + layout->cf_last_log );

	// Create the ringbuffer stream.
	status = dc_rbstream_new (&rbstream, abstract, 1, layout->rbstream_size, layout->rb_profile_begin, layout->rb_profile_end, last_start_address, DC_RBSTREAM_BACKWARD);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		goto error;
	}

	int invalid_profile_flag = 0;

	// Loop through each dive
	for (unsigned int i = 0; i < dive_count; ++i) {
		unsigned int idx = (layout->rb_logbook_entry_count + head_dive - (i + 1)) % layout->rb_logbook_entry_count;

		unsigned char *log_entry = data.logbook + idx * layout->rb_logbook_entry_size;

		unsigned int sample_start_address = 0;
		unsigned int sample_end_address = 0;
		if (layout->model == COCHRAN_MODEL_COMMANDER_TM) {
			sample_start_address = base + array_uint16_le (log_entry + layout->pt_profile_begin);
			sample_end_address = last_start_address;
			// Commander TM has SRAM which seems to randomize when they lose power for too long
			// Check for bad entries.
			if (sample_start_address < layout->rb_profile_begin || sample_start_address > layout->rb_profile_end ||
				sample_end_address < layout->rb_profile_begin || sample_end_address > layout->rb_profile_end ||
				array_uint16_le(log_entry + layout->pt_dive_number) % layout->rb_logbook_entry_count != idx) {
				ERROR(abstract->context, "Corrupt dive (%d).", idx);
				continue;
			}
		} else {
			sample_start_address = base + array_uint32_le (log_entry + layout->pt_profile_begin);
			sample_end_address = base + array_uint32_le (log_entry + layout->pt_profile_end);
		}

		int sample_size = 0, pre_size = 0;

		// Determine if profile exists
		if (idx == data.invalid_profile_dive_num)
			invalid_profile_flag = 1;

		if (!invalid_profile_flag) {
			sample_size = cochran_commander_profile_size(device, &data, idx, sample_start_address, sample_end_address);
			pre_size = cochran_commander_profile_size(device, &data, idx, sample_end_address, last_start_address);
			last_start_address = sample_start_address;
		}

		// Build dive blob
		unsigned int dive_size = layout->rb_logbook_entry_size + sample_size;
		unsigned char *dive = (unsigned char *) malloc(dive_size + pre_size);
		if (dive == NULL) {
			status = DC_STATUS_NOMEMORY;
			goto error;
		}

		memcpy(dive, log_entry, layout->rb_logbook_entry_size); // log

		// Read profile data
		if (sample_size) {
			rc = dc_rbstream_read(rbstream, &progress, dive + layout->rb_logbook_entry_size, sample_size + pre_size);
			if (rc != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to read the sample data.");
				status = rc;
				free(dive);
				goto error;
			}
		}

		if (callback && !callback (dive, dive_size, dive + layout->pt_fingerprint, layout->fingerprint_size, userdata)) {
			free(dive);
			break;
		}

		free(dive);
	}

error:
	dc_rbstream_free(rbstream);
	free(data.logbook);
	return status;
}
