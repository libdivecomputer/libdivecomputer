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
#include "serial.h"
#include "array.h"
#include "ringbuffer.h"

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

#define MAXRETRIES 2

#define COCHRAN_MODEL_COMMANDER_PRE21000 0
#define COCHRAN_MODEL_COMMANDER_AIR_NITROX 1
#define COCHRAN_MODEL_EMC_14 2
#define COCHRAN_MODEL_EMC_16 3
#define COCHRAN_MODEL_EMC_20 4

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
	int fp_dive_num;
	int invalid_profile_dive_num;

	unsigned int logbook_size;

	unsigned int sample_data_offset;
	unsigned int sample_size;
} cochran_data_t;

typedef struct cochran_device_layout_t {
	unsigned int model;
	unsigned int address_bits;
	cochran_endian_t endian;
	unsigned int baudrate;
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
} cochran_device_layout_t;

typedef struct cochran_commander_device_t {
	dc_device_t base;
	dc_serial_t *port;
	const cochran_device_layout_t *layout;
	unsigned char id[67];
	unsigned char fingerprint[6];
} cochran_commander_device_t;

static dc_status_t cochran_commander_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t cochran_commander_device_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t cochran_commander_device_dump (dc_device_t *device, dc_buffer_t *data);
static dc_status_t cochran_commander_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);
static dc_status_t cochran_commander_device_close (dc_device_t *device);

static const dc_device_vtable_t cochran_commander_device_vtable = {
	sizeof (cochran_commander_device_t),
	DC_FAMILY_COCHRAN_COMMANDER,
	cochran_commander_device_set_fingerprint,/* set_fingerprint */
	cochran_commander_device_read, /* read */
	NULL, /* write */
	cochran_commander_device_dump, /* dump */
	cochran_commander_device_foreach, /* foreach */
	cochran_commander_device_close /* close */
};

// Cochran Commander pre-21000 s/n
static const cochran_device_layout_t cochran_cmdr_1_device_layout = {
	COCHRAN_MODEL_COMMANDER_PRE21000, // model
	24,         // address_bits
	ENDIAN_WORD_BE,  // endian
	115200,     // baudrate
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
};


// Cochran Commander Nitrox
static const cochran_device_layout_t cochran_cmdr_device_layout = {
	COCHRAN_MODEL_COMMANDER_AIR_NITROX, // model
	24,         // address_bits
	ENDIAN_WORD_BE,  // endian
	115200,     // baudrate
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
};

// Cochran EMC-14
static const cochran_device_layout_t cochran_emc14_device_layout = {
	COCHRAN_MODEL_EMC_14, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	806400,     // baudrate
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
};

// Cochran EMC-16
static const cochran_device_layout_t cochran_emc16_device_layout = {
	COCHRAN_MODEL_EMC_16, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	806400,     // baudrate
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
};

// Cochran EMC-20
static const cochran_device_layout_t cochran_emc20_device_layout = {
	COCHRAN_MODEL_EMC_20, // model
	32,         // address_bits
	ENDIAN_LE,  // endian
	806400,     // baudrate
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
};


// Determine model descriptor number from model string
static unsigned int
cochran_commander_get_model (cochran_commander_device_t *device)
{
	const cochran_commander_model_t models[] = {
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
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to set the terminal attributes.");
		return status;
	}

	// Set the timeout for receiving data (5000 ms).
	status = dc_serial_set_timeout (device->port, 5000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to set the timeout.");
		return status;
	}

	// Wake up DC and trigger heartbeat
	dc_serial_set_break(device->port, 1);
	dc_serial_sleep(device->port, 16);
	dc_serial_set_break(device->port, 0);

	// Clear old heartbeats
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Wait for heartbeat byte before send
	unsigned char answer = 0;
	status = dc_serial_read(device->port, &answer, 1, NULL);
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
		if (i) dc_serial_sleep(device->port, 16); // 16 ms

		status = dc_serial_write(device->port, command + i, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}
	}

	if (high_speed) {
		// Give the DC time to process the command.
		dc_serial_sleep(device->port, 45);

		// Rates are odd, like 806400 for the EMC, 115200 for commander
		status = dc_serial_configure(device->port, device->layout->baudrate, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
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

		status = dc_serial_read (device->port, answer + nbytes, len, NULL);
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

	// Read two 512 byte blocks into one 1024 byte buffer
	for (unsigned int i = 0; i < 2; i++) {
		const unsigned int len = size / 2;

		unsigned char command[2] = {0x96, i};
		rc = cochran_commander_packet(device, progress, command, sizeof(command), data + i * len, len, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		dc_event_vendor_t vendor;
		vendor.data = data + i * len;
		vendor.size = len;
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
		command[0] = 0x15;
		command[1] = (address      ) & 0xff;
		command[2] = (address >>  8) & 0xff;
		command[3] = (address >> 16) & 0xff;
		command[4] = (size         ) & 0xff;
		command[5] = (size >>  8   ) & 0xff;
		command[6] = (size >> 16   ) & 0xff;
		command[7] = 0x04;
		command_size = 8;
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}

	dc_serial_sleep(device->port, 800);

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

static unsigned int
cochran_commander_read_retry (cochran_commander_device_t *device, dc_event_progress_t *progress, unsigned int address, unsigned char data[], unsigned int size)
{
	// Save the state of the progress events.
	unsigned int saved = progress->current;

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
		progress->current = saved;
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

	return ringbuffer_distance(sample_start_address, sample_end_address, 0, device->layout->rb_profile_begin, device->layout->rb_profile_end);
}


/*
 * Do several things. Find the log that matches the fingerprint,
 * calculate the total read size for progress indicator,
 * Determine the most recent dive without profile data.
 */

static unsigned int
cochran_commander_find_fingerprint(cochran_commander_device_t *device, cochran_data_t *data)
{
	// We track profile ringbuffer usage to determine which dives have profile data
	int profile_capacity_remaining = device->layout->rb_profile_end - device->layout->rb_profile_begin;

	int dive_count = -1;
	data->fp_dive_num = -1;

	// Start at end of log
	if (data->dive_count < device->layout->rb_logbook_entry_count)
		dive_count = data->dive_count;
	else
		dive_count = device->layout->rb_logbook_entry_count;
	dive_count--;

	unsigned int sample_read_size = 0;
	data->invalid_profile_dive_num = -1;

	// Remove the pre-dive events that occur after the last dive
	unsigned int rb_head_ptr = 0;
	if (device->layout->endian == ENDIAN_WORD_BE)
		rb_head_ptr = (array_uint32_word_be(data->config + device->layout->cf_last_log) & 0xfffff000) + 0x2000;
	else
		rb_head_ptr = (array_uint32_le(data->config + device->layout->cf_last_log) & 0xfffff000) + 0x2000;

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
	unsigned int last_profile_end = array_uint32_le(data->logbook + last_profile_idx * device->layout->rb_logbook_entry_size + device->layout->pt_profile_end);
	unsigned int last_profile_pre = 0xFFFFFFFF;

	if (device->layout->endian == ENDIAN_WORD_BE)
		last_profile_pre = array_uint32_word_be(data->config + device->layout->cf_last_log);
	else
		last_profile_pre = array_uint32_le(data->config + device->layout->cf_last_log);

	if (rb_head_ptr > last_profile_end)
		profile_capacity_remaining -= rb_head_ptr - last_profile_end;

	// Loop through dives to find FP, Accumulate profile data size,
	// and find the last dive with invalid profile
	for (unsigned int i = 0; i <= dive_count; ++i) {
		unsigned int idx = (device->layout->rb_logbook_entry_count + head_dive - (i + 1)) % device->layout->rb_logbook_entry_count;

		unsigned char *log_entry = data->logbook + idx * device->layout->rb_logbook_entry_size;

		// We're done if we find the fingerprint
		if (!memcmp(device->fingerprint, log_entry + device->layout->pt_fingerprint, device->layout->fingerprint_size)) {
			data->fp_dive_num = idx;
			break;
		}

		unsigned int profile_pre = array_uint32_le(log_entry + device->layout->pt_profile_pre);
		unsigned int profile_begin = array_uint32_le(log_entry + device->layout->pt_profile_begin);
		unsigned int profile_end = array_uint32_le(log_entry + device->layout->pt_profile_end);

		unsigned int sample_size = cochran_commander_profile_size(device, data, idx, profile_pre, last_profile_pre);
		unsigned int read_size = cochran_commander_profile_size(device, data, idx, profile_begin, profile_end);
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
			sample_read_size += read_size;
		}
	}

	return sample_read_size;
}



static void
cochran_commander_get_sample_parms(cochran_commander_device_t *device, cochran_data_t *data)
{
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int pre_dive_offset = 0, end_dive_offset = 0;

	unsigned int dive_count = 0;
	if (data->dive_count < device->layout->rb_logbook_entry_count)
		dive_count = data->dive_count;
	else
		dive_count = device->layout->rb_logbook_entry_count;

	// Find lowest and highest offsets into sample data
	unsigned int low_offset = 0xFFFFFFFF;
	unsigned int high_offset = 0;

	for (int i = data->fp_dive_num + 1; i < dive_count; i++) {
		pre_dive_offset = array_uint32_le (data->logbook + i * device->layout->rb_logbook_entry_size
				+ device->layout->pt_profile_pre);
		end_dive_offset = array_uint32_le (data->logbook + i * device->layout->rb_logbook_entry_size
				+ device->layout->pt_profile_end);

		// Validate offsets, allow 0xFFFFFFF for end_dive_offset
		// because we handle that as a special case.
		if (pre_dive_offset < device->layout->rb_profile_begin ||
			pre_dive_offset > device->layout->rb_profile_end) {
			ERROR(abstract->context, "Invalid pre-dive offset (%08x) on dive %d.", pre_dive_offset, i);
			continue;
		}

		if (end_dive_offset < device->layout->rb_profile_begin ||
			(end_dive_offset > device->layout->rb_profile_end &&
			end_dive_offset != 0xFFFFFFFF)) {
			ERROR(abstract->context, "Invalid end-dive offset (%08x) on dive %d.", end_dive_offset, i);
			continue;
		}

		// Check for ring buffer wrap-around.
		if (pre_dive_offset > end_dive_offset)
			break;

		if (pre_dive_offset < low_offset)
			low_offset = pre_dive_offset;
		if (end_dive_offset > high_offset && end_dive_offset != 0xFFFFFFFF )
			high_offset = end_dive_offset;
	}

	if (pre_dive_offset > end_dive_offset) {
		high_offset = device->layout->rb_profile_end;
		low_offset = device->layout->rb_profile_begin;
		data->sample_data_offset = low_offset;
		data->sample_size = high_offset - low_offset;
	} else if (low_offset < 0xFFFFFFFF && high_offset > 0) {
		data->sample_data_offset = low_offset;
		data->sample_size = high_offset - data->sample_data_offset;
	} else {
		data->sample_data_offset = 0;
		data->sample_size = 0;
	}
}


dc_status_t
cochran_commander_device_open (dc_device_t **out, dc_context_t *context, const char *name)
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
	device->port = NULL;
	cochran_commander_device_set_fingerprint((dc_device_t *) device, NULL, 0);

	// Open the device.
	status = dc_serial_open (&device->port, device->base.context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to open the serial port.");
		goto error_free;
	}

	status = cochran_commander_serial_setup(device);
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Read ID from the device
	status = cochran_commander_read_id (device, device->id, sizeof(device->id));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Device not responding.");
		goto error_close;
	}

	unsigned int model = cochran_commander_get_model(device);
	switch (model) {
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
		goto error_close;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}

static dc_status_t
cochran_commander_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

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

	return cochran_commander_read(device, NULL, address, data, size);
}


static dc_status_t
cochran_commander_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	cochran_commander_device_t *device = (cochran_commander_device_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char config[1024];

	// Make sure buffer is good.
	if (!dc_buffer_clear(buffer)) {
		ERROR (abstract->context, "Uninitialized buffer.");
		return DC_STATUS_INVALIDARGS;
	}

	// Reserve space
	if (!dc_buffer_resize(buffer, device->layout->rb_profile_end)) {
		ERROR(abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Determine size for progress
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = sizeof(config) + device->layout->rb_profile_end;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit ID block
	dc_event_vendor_t vendor;
	vendor.data = device->id;
	vendor.size = sizeof (device->id);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	rc = cochran_commander_read_config (device, &progress, config, sizeof(config));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the sample data, from 0 to sample end will include logbook
	rc = cochran_commander_read (device, &progress, 0, dc_buffer_get_data(buffer), device->layout->rb_profile_end);
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

	cochran_data_t data;
	data.logbook = NULL;

	// Calculate max data sizes
	unsigned int max_config = sizeof(data.config);
	unsigned int max_logbook = layout->rb_logbook_end - layout->rb_logbook_begin;
	unsigned int max_sample = layout->rb_profile_end - layout->rb_profile_begin;

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
	rc = cochran_commander_read_config(device, &progress, data.config, sizeof(data.config));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Determine size of dive list to read.
	if (layout->endian == ENDIAN_LE)
		data.dive_count = array_uint16_le (data.config + layout->cf_dive_count);
	else
		data.dive_count = array_uint16_be (data.config + layout->cf_dive_count);

	if (data.dive_count == 0)
		// No dives to read
		return DC_STATUS_SUCCESS;

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
	rc = cochran_commander_read(device, &progress, 0, data.logbook, data.logbook_size);
	if (rc != DC_STATUS_SUCCESS) {
		status = rc;
		goto error;
	}

	// Locate fingerprint, recent dive with invalid profile and calc read size
	unsigned int profile_read_size = cochran_commander_find_fingerprint(device, &data);
	// Update progress indicator with new maximum
	progress.maximum -= (max_sample - profile_read_size);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	cochran_commander_get_sample_parms(device, &data);

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
	if (data.fp_dive_num > -1)
		tail_dive = (data.fp_dive_num + 1) % layout->rb_logbook_entry_count;

	// Number of dives to read
	dive_count = (layout->rb_logbook_entry_count + head_dive - tail_dive) % layout->rb_logbook_entry_count;

	int invalid_profile_flag = 0;

	// Loop through each dive
	for (unsigned int i = 0; i < dive_count; ++i) {
		unsigned int idx = (layout->rb_logbook_entry_count + head_dive - (i + 1)) % layout->rb_logbook_entry_count;

		unsigned char *log_entry = data.logbook + idx * layout->rb_logbook_entry_size;

		unsigned int sample_start_address = array_uint32_le (log_entry + layout->pt_profile_begin);
		unsigned int sample_end_address = array_uint32_le (log_entry + layout->pt_profile_end);

		int sample_size = 0;

		// Determine if profile exists
		if (idx == data.invalid_profile_dive_num)
			invalid_profile_flag = 1;

		if (!invalid_profile_flag)
			sample_size = cochran_commander_profile_size(device, &data, idx, sample_start_address, sample_end_address);

		// Build dive blob
		unsigned int dive_size = layout->rb_logbook_entry_size + sample_size;
		unsigned char *dive = (unsigned char *) malloc(dive_size);
		if (dive == NULL) {
			status = DC_STATUS_NOMEMORY;
			goto error;
		}

		memcpy(dive, log_entry, layout->rb_logbook_entry_size); // log

		// Read profile data
		if (sample_size) {
			if (sample_end_address == 0xFFFFFFFF)
				// Corrupt dive, guess the end address
				sample_end_address = cochran_commander_guess_sample_end_address(device, &data, idx);

			if (sample_start_address <= sample_end_address) {
				rc = cochran_commander_read_retry (device, &progress, sample_start_address, dive + layout->rb_logbook_entry_size, sample_size);
				if (rc != DC_STATUS_SUCCESS) {
					ERROR (abstract->context, "Failed to read the sample data.");
					status = rc;
					free(dive);
					goto error;
				}
			} else {
				// It wrapped the buffer, copy two sections
				unsigned int size = layout->rb_profile_end - sample_start_address;

				rc = cochran_commander_read_retry (device, &progress, sample_start_address, dive + layout->rb_logbook_entry_size, size);
				if (rc != DC_STATUS_SUCCESS) {
					ERROR (abstract->context, "Failed to read the sample data.");
					status = rc;
					free(dive);
					goto error;
				}

				rc = cochran_commander_read_retry (device, &progress, layout->rb_profile_begin, dive + layout->rb_logbook_entry_size + size, sample_end_address - layout->rb_profile_begin);
				if (rc != DC_STATUS_SUCCESS) {
					ERROR (abstract->context, "Failed to read the sample data.");
					status = rc;
					free(dive);
					goto error;
				}
			}
		}

		if (callback && !callback (dive, dive_size, dive + layout->pt_fingerprint, layout->fingerprint_size, userdata)) {
			free(dive);
			break;
		}

		free(dive);
	}

error:
	free(data.logbook);
	return status;
}
