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

#include <stdlib.h>
#include <math.h>

#include <libdivecomputer/units.h>

#include "cochran_commander.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"
#include "array.h"

#define COCHRAN_MODEL_COMMANDER_TM 0
#define COCHRAN_MODEL_COMMANDER_PRE21000 1
#define COCHRAN_MODEL_COMMANDER_AIR_NITROX 2
#define COCHRAN_MODEL_EMC_14 3
#define COCHRAN_MODEL_EMC_16 4
#define COCHRAN_MODEL_EMC_20 5

// Cochran time stamps start at Jan 1, 1992
#define COCHRAN_EPOCH 694242000

#define UNSUPPORTED 0xFFFFFFFF

typedef enum cochran_sample_format_t {
	SAMPLE_TM,
	SAMPLE_CMDR,
	SAMPLE_EMC,
} cochran_sample_format_t;


typedef enum cochran_date_encoding_t {
	DATE_ENCODING_MSDHYM,
	DATE_ENCODING_SMHDMY,
	DATE_ENCODING_TICKS,
} cochran_date_encoding_t;

typedef struct cochran_parser_layout_t {
	cochran_sample_format_t format;
	unsigned int headersize;
	unsigned int samplesize;
	unsigned int pt_sample_interval;
	cochran_date_encoding_t date_encoding;
	unsigned int datetime;
	unsigned int pt_profile_begin;
	unsigned int water_conductivity;
	unsigned int pt_profile_pre;
	unsigned int start_temp;
	unsigned int start_depth;
	unsigned int dive_number;
	unsigned int altitude;
	unsigned int pt_profile_end;
	unsigned int end_temp;
	unsigned int divetime;
	unsigned int max_depth;
	unsigned int avg_depth;
	unsigned int oxygen;
	unsigned int helium;
	unsigned int min_temp;
	unsigned int max_temp;
} cochran_parser_layout_t;

typedef struct cochran_events_t {
	unsigned char code;
	unsigned int data_bytes;
	parser_sample_event_t type;
	parser_sample_flags_t flag;
} cochran_events_t;

typedef struct event_size_t {
	unsigned int code;
	unsigned int size;
} event_size_t;

typedef struct cochran_commander_parser_t {
	dc_parser_t base;
	unsigned int model;
	const cochran_parser_layout_t *layout;
	const event_size_t *events;
	unsigned int nevents;
} cochran_commander_parser_t ;

static dc_status_t cochran_commander_parser_get_datetime (dc_parser_t *parser, dc_datetime_t *datetime);
static dc_status_t cochran_commander_parser_get_field (dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cochran_commander_parser_samples_foreach (dc_parser_t *parser, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t cochran_commander_parser_vtable = {
	sizeof(cochran_commander_parser_t),
	DC_FAMILY_COCHRAN_COMMANDER,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	cochran_commander_parser_get_datetime, /* datetime */
	cochran_commander_parser_get_field, /* fields */
	cochran_commander_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const cochran_parser_layout_t cochran_cmdr_tm_parser_layout = {
	SAMPLE_TM,   // format
	90,          // headersize
	1,           // samplesize
	72,          // pt_sample_interval
	DATE_ENCODING_TICKS, // date_encoding
	15,          // datetime, 4 bytes
	0,           // pt_profile_begin, 4 bytes
	UNSUPPORTED, // water_conductivity, 1 byte, 0=low(fresh), 2=high(sea)
	0,           // pt_profile_pre, 4 bytes
	83,          // start_temp, 1 byte, F
	UNSUPPORTED, // start_depth, 2 bytes, /4=ft
	20,          // dive_number, 2 bytes
	UNSUPPORTED, // altitude, 1 byte, /4=kilofeet
	UNSUPPORTED, // pt_profile_end, 4 bytes
	UNSUPPORTED, // end_temp, 1 byte F
	57,          // divetime, 2 bytes, minutes
	49,          // max_depth, 2 bytes, /4=ft
	51,          // avg_depth, 2 bytes, /4=ft
	74,          // oxygen, 4 bytes (2 of) 2 bytes, /256=%
	UNSUPPORTED, // helium, 4 bytes (2 of) 2 bytes, /256=%
	82,          // min_temp, 1 byte, /2+20=F
	UNSUPPORTED, // max_temp, 1 byte, /2+20=F
};

static const cochran_parser_layout_t cochran_cmdr_1_parser_layout = {
	SAMPLE_CMDR, // format
	256,         // headersize
	2,           // samplesize
	UNSUPPORTED, // pt_sample_interval
	DATE_ENCODING_TICKS, // date_encoding
	8,           // datetime, 4 bytes
	0,           // pt_profile_begin, 4 bytes
	24,          // water_conductivity, 1 byte, 0=low(fresh), 2=high(sea)
	28,          // pt_profile_pre, 4 bytes
	43,          // start_temp, 1 byte, F
	54,          // start_depth, 2 bytes, /4=ft
	68,          // dive_number, 2 bytes
	73,          // altitude, 1 byte, /4=kilofeet
	128,         // pt_profile_end, 4 bytes
	153,         // end_temp, 1 byte F
	166,         // divetime, 2 bytes, minutes
	168,         // max_depth, 2 bytes, /4=ft
	170,         // avg_depth, 2 bytes, /4=ft
	210,         // oxygen, 4 bytes (2 of) 2 bytes, /256=%
	UNSUPPORTED, // helium, 4 bytes (2 of) 2 bytes, /256=%
	232,         // min_temp, 1 byte, /2+20=F
	233,         // max_temp, 1 byte, /2+20=F
};

static const cochran_parser_layout_t cochran_cmdr_parser_layout = {
	SAMPLE_CMDR, // format
	256,         // headersize
	2,           // samplesize
	UNSUPPORTED, // pt_sample_interval
	DATE_ENCODING_MSDHYM, // date_encoding
	0,           // datetime, 6 bytes
	6,           // pt_profile_begin, 4 bytes
	24,          // water_conductivity, 1 byte, 0=low(fresh), 2=high(sea)
	30,          // pt_profile_pre, 4 bytes
	45,          // start_temp, 1 byte, F
	56,          // start_depth, 2 bytes, /4=ft
	70,          // dive_number, 2 bytes
	73,          // altitude, 1 byte, /4=kilofeet
	128,         // pt_profile_end, 4 bytes
	153,         // end_temp, 1 byte F
	166,         // divetime, 2 bytes, minutes
	168,         // max_depth, 2 bytes, /4=ft
	170,         // avg_depth, 2 bytes, /4=ft
	210,         // oxygen, 4 bytes (2 of) 2 bytes, /256=%
	UNSUPPORTED, // helium, 4 bytes (2 of) 2 bytes, /256=%
	232,         // min_temp, 1 byte, /2+20=F
	233,         // max_temp, 1 byte, /2+20=F
};

static const cochran_parser_layout_t cochran_emc_parser_layout = {
	SAMPLE_EMC,  // format
	512,         // headersize
	3,           // samplesize
	UNSUPPORTED, // pt_sample_interval
	DATE_ENCODING_SMHDMY, // date_encoding
	0,           // datetime, 6 bytes
	6,           // pt_profile_begin, 4 bytes
	24,          // water_conductivity, 1 byte 0=low(fresh), 2=high(sea)
	30,          // pt_profile_pre, 4 bytes
	55,          // start_temp, 1 byte, F
	42,          // start_depth, 2 bytes, /256=ft
	86,          // dive_number, 2 bytes,
	89,          // altitude, 1 byte /4=kilofeet
	256,         // pt_profile_end, 4 bytes
	293,         // end_temp, 1 byte, F
	304,         // divetime, 2 bytes, minutes
	306,         // max_depth, 2 bytes, /4=ft
	310,         // avg_depth, 2 bytes, /4=ft
	144,         // oxygen, 6 bytes (3 of) 2 bytes, /256=%
	164,         // helium, 6 bytes (3 of) 2 bytes, /256=%
	403,         // min_temp, 1 byte, /2+20=F
	407,         // max_temp, 1 byte, /2+20=F
};

static const cochran_events_t cochran_events[] = {
	{0xA8, 1, SAMPLE_EVENT_SURFACE,  SAMPLE_FLAGS_BEGIN}, // Entered PDI mode
	{0xA9, 1, SAMPLE_EVENT_SURFACE,  SAMPLE_FLAGS_END},   // Exited PDI mode
	{0xAB, 5, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Ceiling decrease
	{0xAD, 5, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Ceiling increase
	{0xB5, 1, SAMPLE_EVENT_AIRTIME,  SAMPLE_FLAGS_BEGIN}, // Air < 5 mins deco
	{0xBD, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switched to nomal PO2 setting
	{0xBE, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Ceiling > 60 ft
	{0xC0, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switched to FO2 21% mode
	{0xC1, 1, SAMPLE_EVENT_ASCENT,   SAMPLE_FLAGS_BEGIN}, // Ascent rate greater than limit
	{0xC2, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Low battery warning
	{0xC3, 1, SAMPLE_EVENT_OLF,      SAMPLE_FLAGS_NONE},  // CNS Oxygen toxicity warning
	{0xC4, 1, SAMPLE_EVENT_MAXDEPTH, SAMPLE_FLAGS_NONE},  // Depth exceeds user set point
	{0xC5, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_BEGIN}, // Entered decompression mode
	{0xC7, 1, SAMPLE_EVENT_VIOLATION,SAMPLE_FLAGS_BEGIN}, // Entered Gauge mode (e.g. locked out)
	{0xC8, 1, SAMPLE_EVENT_PO2,      SAMPLE_FLAGS_BEGIN}, // PO2 too high
	{0xCC, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_BEGIN}, // Low Cylinder 1 pressure
	{0xCE, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_BEGIN}, // Non-decompression warning
	{0xCF, 1, SAMPLE_EVENT_OLF,      SAMPLE_FLAGS_BEGIN}, // O2 Toxicity
	{0xCD, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switched to deco blend
	{0xD0, 1, SAMPLE_EVENT_WORKLOAD, SAMPLE_FLAGS_BEGIN}, // Breathing rate alarm
	{0xD3, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Low gas 1 flow rate
	{0xD6, 1, SAMPLE_EVENT_CEILING,  SAMPLE_FLAGS_BEGIN}, // Depth is less than ceiling
	{0xD8, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_END},   // End decompression mode
	{0xE1, 1, SAMPLE_EVENT_ASCENT,   SAMPLE_FLAGS_END},   // End ascent rate warning
	{0xE2, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Low SBAT battery warning
	{0xE3, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switched to FO2 mode
	{0xE5, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switched to PO2 mode
	{0xEE, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_END},   // End non-decompresison warning
	{0xEF, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switch to blend 2
	{0xF0, 1, SAMPLE_EVENT_WORKLOAD, SAMPLE_FLAGS_END},   // Breathing rate alarm
	{0xF3, 1, SAMPLE_EVENT_NONE,     SAMPLE_FLAGS_NONE},  // Switch to blend 1
	{0xF6, 1, SAMPLE_EVENT_CEILING,  SAMPLE_FLAGS_END},   // End Depth is less than ceiling
};

static const event_size_t cochran_cmdr_event_bytes[] = {
	{0x00, 17}, {0x01, 21}, {0x02, 18},
	{0x03, 17}, {0x06, 19}, {0x07, 19},
	{0x08, 19}, {0x09, 19}, {0x0a, 19},
	{0x0b, 21}, {0x0c, 19}, {0x0d, 19},
	{0x0e, 19}, {0x10, 21},
};

static const event_size_t cochran_emc_event_bytes[] = {
	{0x00, 19}, {0x01, 23}, {0x02, 20},
	{0x03, 19}, {0x06, 21}, {0x07, 21},
	{0x0a, 21}, {0x0b, 21}, {0x0f, 19},
	{0x10, 21},
};


static unsigned int
cochran_commander_handle_event (cochran_commander_parser_t *parser, unsigned char code, dc_sample_callback_t callback, void *userdata)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;

	const cochran_events_t *event = NULL;
	for (unsigned int i = 0; i < C_ARRAY_SIZE(cochran_events); ++i) {
		if (cochran_events[i].code == code) {
			event = cochran_events + i;
			break;
		}
	}

	if (event == NULL) {
		// Unknown event, send warning so we know we missed something
		WARNING(abstract->context, "Unknown event 0x%02x", code);
		return 1;
	}

	switch (code) {
	case 0xAB: // Ceiling decrease
		// Indicated to lower ceiling by 10 ft (deeper)
		// Bytes 1-2: first stop duration (min)
		// Bytes 3-4: total stop duration (min)
		// Handled in calling function
		break;
	case 0xAD: // Ceiling increase
		// Indicates to raise ceiling by 10 ft (shallower)
		// Handled in calling function
		break;
	case 0xC0: // Switched to FO2 21% mode (surface)
		// Event seen upon surfacing
		// handled in calling function
		break;
	case 0xCD: // Switched to deco blend
	case 0xEF: // Switched to gas blend 2
	case 0xF3: // Switched to gas blend 1
		// handled in calling function
		break;
	default:
		// Don't send known events of type NONE
		if (event->type != SAMPLE_EVENT_NONE) {
			dc_sample_value_t sample = {0};
			sample.event.type = event->type;
			sample.event.time = 0;
			sample.event.value = 0;
			sample.event.flags = event->flag;
			if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
		}
	}

	return event->data_bytes;
}


/*
 * Used to find the end of a dive that has an incomplete dive-end
 * block. It parses backwards past inter-dive events.
 */
static int
cochran_commander_backparse(cochran_commander_parser_t *parser, const unsigned char *samples, int size)
{
	int result = size, best_result = size;

	for (unsigned int i = 0; i < parser->nevents; i++) {
		int ptr = size - parser->events[i].size;
		if (ptr > 0 && samples[ptr] == parser->events[i].code) {
			// Recurse to find the largest match. Because we are parsing backwards
			// and the events vary in size we can't be sure the byte that matches
			// the event code is an event code or data from inside a longer or shorter
			// event.
			result = cochran_commander_backparse(parser, samples, ptr);
		}

		if (result < best_result) {
			best_result = result;
		}
	}

	return best_result;
}


dc_status_t
cochran_commander_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	cochran_commander_parser_t *parser = NULL;
	dc_status_t status = DC_STATUS_SUCCESS;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (cochran_commander_parser_t *) dc_parser_allocate (context, &cochran_commander_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->model = model;

	switch (model) {
	case COCHRAN_MODEL_COMMANDER_TM:
		parser->layout = &cochran_cmdr_tm_parser_layout;
		parser->events = NULL;	// No inter-dive events on this model
		parser->nevents = 0;
		break;
	case COCHRAN_MODEL_COMMANDER_PRE21000:
		parser->layout = &cochran_cmdr_1_parser_layout;
		parser->events = cochran_cmdr_event_bytes;
		parser->nevents = C_ARRAY_SIZE(cochran_cmdr_event_bytes);
		break;
	case COCHRAN_MODEL_COMMANDER_AIR_NITROX:
		parser->layout = &cochran_cmdr_parser_layout;
		parser->events = cochran_cmdr_event_bytes;
		parser->nevents = C_ARRAY_SIZE(cochran_cmdr_event_bytes);
		break;
	case COCHRAN_MODEL_EMC_14:
	case COCHRAN_MODEL_EMC_16:
	case COCHRAN_MODEL_EMC_20:
		parser->layout = &cochran_emc_parser_layout;
		parser->events = cochran_emc_event_bytes;
		parser->nevents = C_ARRAY_SIZE(cochran_emc_event_bytes);
		break;
	default:
		status = DC_STATUS_UNSUPPORTED;
		goto error_free;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;

error_free:
	dc_parser_deallocate ((dc_parser_t *) parser);
	return status;
}


static dc_status_t
cochran_commander_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	cochran_commander_parser_t *parser = (cochran_commander_parser_t *) abstract;
	const cochran_parser_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;

	if (abstract->size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	dc_ticks_t ts = 0;

	if (datetime) {
		switch (layout->date_encoding)
		{
		case DATE_ENCODING_MSDHYM:
			datetime->second = data[layout->datetime + 1];
			datetime->minute = data[layout->datetime + 0];
			datetime->hour = data[layout->datetime + 3];
			datetime->day = data[layout->datetime + 2];
			datetime->month = data[layout->datetime + 5];
			datetime->year = data[layout->datetime + 4] + (data[layout->datetime + 4] > 91 ? 1900 : 2000);
			datetime->timezone = DC_TIMEZONE_NONE;
			break;
		case DATE_ENCODING_SMHDMY:
			datetime->second = data[layout->datetime + 0];
			datetime->minute = data[layout->datetime + 1];
			datetime->hour = data[layout->datetime + 2];
			datetime->day = data[layout->datetime + 3];
			datetime->month = data[layout->datetime + 4];
			datetime->year = data[layout->datetime + 5] + (data[layout->datetime + 5] > 91 ? 1900 : 2000);
			datetime->timezone = DC_TIMEZONE_NONE;
			break;
		case DATE_ENCODING_TICKS:
			ts = array_uint32_le(data + layout->datetime) + COCHRAN_EPOCH;
			dc_datetime_localtime(datetime, ts);
			break;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const cochran_commander_parser_t *parser = (cochran_commander_parser_t *) abstract;
	const cochran_parser_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;
	unsigned int minutes = 0, qfeet = 0;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;

	if (abstract->size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	if (value) {
		switch (type) {
		case DC_FIELD_TEMPERATURE_SURFACE:
			*((double *) value) = (data[layout->start_temp] - 32.0) / 1.8;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			if (data[layout->min_temp] == 0xFF)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = (data[layout->min_temp] / 2.0 + 20 - 32) / 1.8;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			if (layout->max_temp == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			if (data[layout->max_temp] == 0xFF)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = (data[layout->max_temp] / 2.0 + 20 - 32) / 1.8;
			break;
		case DC_FIELD_DIVETIME:
			minutes = array_uint16_le(data + layout->divetime);
			if (minutes == 0xFFFF)
				return DC_STATUS_UNSUPPORTED;
			*((unsigned int *) value) = minutes * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			qfeet = array_uint16_le(data + layout->max_depth);
			if (qfeet == 0xFFFF)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = qfeet / 4.0 * FEET;
			break;
		case DC_FIELD_AVGDEPTH:
			qfeet = array_uint16_le(data + layout->avg_depth);
			if (qfeet == 0xFFFF)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = qfeet / 4.0 * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 2;
			break;
		case DC_FIELD_GASMIX:
			// Gas percentages are decimal and encoded as
			// highbyte = integer portion
			// lowbyte = decimal portion, divide by 256 to get decimal value
			gasmix->usage = DC_USAGE_NONE;
			gasmix->oxygen = array_uint16_le (data + layout->oxygen + 2 * flags) / 256.0 / 100;
			if (layout->helium == UNSUPPORTED) {
				gasmix->helium = 0;
			} else {
				gasmix->helium = array_uint16_le (data + layout->helium + 2 * flags) / 256.0 / 100;
			}
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			// 0x00 = low conductivity, 0x10 = high, maybe there's a 0x01 and 0x11?
			// Assume Cochran's conductivity ranges from 0 to 3
			// 0 is fresh water, anything else is sea water
			// for density assume
			//  0 = 1000kg/m³, 2 = 1025kg/m³
			// and other values are linear
			if (layout->water_conductivity == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			if ((data[layout->water_conductivity] & 0x3) == 0)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = 1000.0 + 12.5 * (data[layout->water_conductivity] & 0x3);
			break;
		case DC_FIELD_ATMOSPHERIC:
			// Cochran measures air pressure and stores it as altitude.
			// Convert altitude (measured in 1/4 kilofeet) back to pressure.
			if (layout->altitude == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			*(double *) value = ATM / BAR * pow(1 - 0.0000225577 * data[layout->altitude] * 250.0 * FEET, 5.25588);
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


/*
 * Parse early Commander computers
 */
static dc_status_t
cochran_commander_parser_samples_foreach_tm (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	cochran_commander_parser_t *parser = (cochran_commander_parser_t *) abstract;
	const cochran_parser_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;
	const unsigned char *samples = data + layout->headersize;

	if (abstract->size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int size = abstract->size - layout->headersize;
	unsigned int sample_interval = data[layout->pt_sample_interval];

	dc_sample_value_t sample = {0};
	unsigned int time = 0, last_sample_time = 0;
	unsigned int offset = 2;
	unsigned int deco_ceiling = 0;

	unsigned int temp = samples[0];	// Half degrees F
	unsigned int depth = samples[1];	// Half feet

	last_sample_time = sample.time = time * 1000;
	if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

	sample.depth = (depth / 2.0) * FEET;
	if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

	sample.temperature = (temp / 2.0 - 32.0) / 1.8;
	if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

	sample.gasmix = 0;
	if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);

	while (offset < size) {
		const unsigned char *s = samples + offset;

		sample.time = time * 1000;
		if (last_sample_time != sample.time) {
			// We haven't issued this time yet.
			last_sample_time = sample.time;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
		}

		if (*s & 0x80) {
			// Event or temperate change byte
			if (*s & 0x60) {
				// Event byte
				switch (*s) {
				case 0xC5:  // Deco obligation begins
					break;
				case 0xD8:  // Deco obligation ends
					break;
				case 0xAB:  // Decrement ceiling (deeper)
					deco_ceiling += 10; // feet

					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.time = 60; // We don't know the duration
					sample.deco.depth = deco_ceiling * FEET;
					sample.deco.tts = 0;
					if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);
					break;
				case 0xAD:  // Increment ceiling (shallower)
					deco_ceiling -= 10; // feet

					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.depth = deco_ceiling * FEET;
					sample.deco.time = 60; // We don't know the duration
					sample.deco.tts = 0;
					if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);
					break;
				default:
					cochran_commander_handle_event(parser, s[0], callback, userdata);
					break;
				}
			} else {
				// Temp change
				if (*s & 0x10)
					temp -= (*s & 0x0f);
				else
					temp += (*s & 0x0f);
				sample.temperature = (temp / 2.0 - 32.0) / 1.8;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}

			offset++;
			continue;
		}

		// Depth sample
		if (s[0] & 0x40)
			depth -= s[0] & 0x3f;
		else
			depth += s[0] & 0x3f;

		sample.depth = (depth / 2.0) * FEET;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		offset++;
		time += sample_interval;
	}
	return DC_STATUS_SUCCESS;
}


/*
 * Parse Commander I (Pre-21000 s/n), II and EMC computers
 */
static dc_status_t
cochran_commander_parser_samples_foreach_emc (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	cochran_commander_parser_t *parser = (cochran_commander_parser_t *) abstract;
	const cochran_parser_layout_t *layout = parser->layout;
	const unsigned char *data = abstract->data;
	const unsigned char *samples = data + layout->headersize;
	const unsigned char *last_sample = NULL;

	if (abstract->size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int size = abstract->size - layout->headersize;

	dc_sample_value_t sample = {0};
	unsigned int time = 0, last_sample_time = 0;
	unsigned int offset = 0;
	double start_depth = 0;
	int depth = 0;
	unsigned int deco_obligation = 0;
	unsigned int deco_ceiling = 0;
	unsigned int corrupt_dive = 0;

	// In rare circumstances Cochran computers won't record the end-of-dive
	// log entry block. When the end-sample pointer is 0xFFFFFFFF it's corrupt.
	// That means we don't really know where the dive samples end and we don't
	// know what the dive summary values are (i.e. max depth, min temp)
	if (array_uint32_le(data + layout->pt_profile_end) == 0xFFFFFFFF) {
		corrupt_dive = 1;
		dc_datetime_t d;
		cochran_commander_parser_get_datetime(abstract, &d);

		WARNING(abstract->context, "Incomplete dive on %02d/%02d/%02d at %02d:%02d:%02d, trying to parse samples",
				d.year, d.month, d.day, d.hour, d.minute, d.second);

		// Eliminate inter-dive events
		size = cochran_commander_backparse(parser, samples, size);
	}

	// Cochran samples depth every second and varies between ascent rate
	// and temp every other second.

	// Prime values from the dive log section
	if (parser->model == COCHRAN_MODEL_COMMANDER_AIR_NITROX ||
		parser->model == COCHRAN_MODEL_COMMANDER_PRE21000) {
		// Commander stores start depth in quarter-feet
		start_depth = array_uint16_le (data + layout->start_depth) / 4.0;
	} else {
		// EMC stores start depth in 256ths of a foot.
		start_depth = array_uint16_le (data + layout->start_depth) / 256.0;
	}

	last_sample_time = sample.time = time * 1000;
	if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

	sample.depth = start_depth * FEET;
	if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

	sample.temperature = (data[layout->start_temp] - 32.0) / 1.8;
	if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

	sample.gasmix = 0;
	if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
	unsigned int last_gasmix = sample.gasmix;

	while (offset < size) {
		const unsigned char *s = samples + offset;

		sample.time = time * 1000;
		if (last_sample_time != sample.time) {
			// We haven't issued this time yet.
			last_sample_time = sample.time;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);
		}

		// If corrupt_dive end before offset
		if (corrupt_dive) {
			// When we aren't sure where the sample data ends we can
			// look for events that shouldn't be in the sample data.
			// 0xFF is unwritten memory
			// 0xA8 indicates start of post-dive interval
			// 0xE3 (switch to FO2 mode) and 0xF3 (switch to blend 1) occur
			// at dive start so when we see them after the first second we
			// found the beginning of the next dive.
			if (s[0] == 0xFF || s[0] == 0xA8) {
				DEBUG(abstract->context, "Used corrupt dive breakout 1 on event %02x", s[0]);
				break;
			}
			if (time > 1 && (s[0] == 0xE3 || s[0] == 0xF3)) {
				DEBUG(abstract->context, "Used corrupt dive breakout 2 on event %02x", s[0]);
				break;
			}
		}

		// Check for event
		if (s[0] & 0x80) {
			offset += cochran_commander_handle_event(parser, s[0], callback, userdata);

			// Events indicating change in deco status
			switch (s[0]) {
			case 0xC5:  // Deco obligation begins
				deco_obligation = 1;
				break;
			case 0xD8:  // Deco obligation ends
				deco_obligation = 0;
				break;
			case 0xAB:  // Decrement ceiling (deeper)
				deco_ceiling += 10; // feet

				sample.deco.type = DC_DECO_DECOSTOP;
				sample.deco.time = (array_uint16_le(s + 3) + 1) * 60;
				sample.deco.depth = deco_ceiling * FEET;
				sample.deco.tts = 0;
				if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);
				break;
			case 0xAD:  // Increment ceiling (shallower)
				deco_ceiling -= 10; // feet

				sample.deco.type = DC_DECO_DECOSTOP;
				sample.deco.depth = deco_ceiling * FEET;
				sample.deco.time = (array_uint16_le(s + 3) + 1) * 60;
				sample.deco.tts = 0;
				if (callback) callback(DC_SAMPLE_DECO, &sample, userdata);
				break;
			case 0xC0:  // Switched to FO2 21% mode (surface)
				// Event seen upon surfacing
				break;
			case 0xCD:  // Switched to deco blend
			case 0xEF:  // Switched to gas blend 2
				if (last_gasmix != 1) {
					sample.gasmix = 1;
					if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
					last_gasmix = sample.gasmix;
				}
				break;
			case 0xF3:  // Switched to gas blend 1
				if (last_gasmix != 0) {
					sample.gasmix = 0;
					if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
					last_gasmix = sample.gasmix;
				}
				break;
			}

			continue;
		}

		// Make sure we have a full sample
		if (offset + layout->samplesize > size)
			break;

		// Depth is logged as change in feet, bit 0x40 means negative depth
		if (s[0] & 0x40)
			depth -= (s[0] & 0x3f);
		else
			depth += (s[0] & 0x3f);

		sample.depth = (start_depth + depth / 4.0) * FEET;
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

		// Ascent rate is logged in the 0th sample, temp in the 1st, repeat.
		if (time % 2 == 0) {
			// Ascent rate
			double DC_ATTR_UNUSED ascent_rate = 0.0;
			if (s[1] & 0x80)
				ascent_rate = (s[1] & 0x7f);
			else
				ascent_rate = -(s[1] & 0x7f);
			ascent_rate *= FEET / 4.0;
		} else {
			// Temperature logged in half degrees F above 20
			double temperature = s[1] / 2.0 + 20.0;
			sample.temperature = (temperature - 32.0) / 1.8;

			if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
		}

		// Cochran EMC models store NDL and deco stop time
		// in the 20th to 23rd sample
		if (layout->format == SAMPLE_EMC) {
			// Tissue load is recorded across 20 samples, we ignore them
			// NDL and deco stop time is recorded across the next 4 samples
			// The first 2 are either NDL or stop time at deepest stop (if in deco)
			// The next 2 are total deco stop time.
			unsigned int deco_time = 0;
			switch (time % 24) {
			case 21:
				deco_time = last_sample[2] + s[2] * 256 + 1;
				if (deco_obligation) {
					/* Deco time for deepest stop, unused */
				} else {
					/* Send deco NDL sample */
					sample.deco.type = DC_DECO_NDL;
					sample.deco.time = deco_time * 60;
					sample.deco.depth = 0;
					sample.deco.tts = 0;
					if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
				}
				break;
			case 23:
				/* Deco time, total obligation */
				deco_time = last_sample[2] + s[2] * 256 + 1;
				if (deco_obligation) {
					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.depth = deco_ceiling * FEET;
					sample.deco.time = deco_time * 60;
					sample.deco.tts = 0;
					if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
				}
				break;
			}
			last_sample = s;
		}

		time++;
		offset += layout->samplesize;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cochran_commander_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	cochran_commander_parser_t *parser = (cochran_commander_parser_t *) abstract;

	if (parser->model == COCHRAN_MODEL_COMMANDER_TM)
		return cochran_commander_parser_samples_foreach_tm (abstract, callback, userdata);
	else
		return cochran_commander_parser_samples_foreach_emc (abstract, callback, userdata);
}
