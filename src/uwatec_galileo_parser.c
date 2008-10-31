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

#include <stdlib.h>
#include <string.h>	// memcmp
#include <assert.h>

#include "uwatec_smart.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define NBITS 8
#define NELEMENTS(x) ( sizeof(x) / sizeof((x)[0]) )

typedef struct uwatec_galileo_parser_t uwatec_galileo_parser_t;

struct uwatec_galileo_parser_t {
	parser_t base;
	unsigned int model;
};

static parser_status_t uwatec_galileo_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t uwatec_galileo_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t uwatec_galileo_parser_destroy (parser_t *abstract);

static const parser_backend_t uwatec_galileo_parser_backend = {
	PARSER_TYPE_UWATEC_GALILEO,
	uwatec_galileo_parser_set_data, /* set_data */
	uwatec_galileo_parser_samples_foreach, /* samples_foreach */
	uwatec_galileo_parser_destroy /* destroy */
};


static int
parser_is_uwatec_galileo (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_galileo_parser_backend;
}


parser_status_t
uwatec_galileo_parser_create (parser_t **out, unsigned int model)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	uwatec_galileo_parser_t *parser = malloc (sizeof (uwatec_galileo_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &uwatec_galileo_parser_backend);

	// Set the default values.
	parser->model = model;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
uwatec_galileo_parser_destroy (parser_t *abstract)
{
	if (! parser_is_uwatec_galileo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
uwatec_galileo_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_uwatec_galileo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static unsigned int
uwatec_galileo_fixsignbit (unsigned int x, unsigned int n)
{
	assert (n > 0);

	unsigned int signbit = (1 << (n - 1));
	unsigned int mask = (0xFFFFFFFF << n);

	// When turning a two's-complement number with a certain number
	// of bits into one with more bits, the sign bit must be repeated
	// in all the extra bits.
	if ((x & signbit) == signbit)
		return x | mask;
	else
		return x & ~mask;
}


static unsigned int
uwatec_galileo_identify (const unsigned char data[], unsigned int size)
{
	assert (size > 0);

	unsigned char value = data[0];

	if ((value & 0x80) == 0) // Delta Depth
		return 0;

	if ((value & 0xE0) == 0x80) // Delta RBT
		return 1;

	switch (value & 0xF0) {
	case 0xA0: // Delta Tank Pressure
		return 2;
	case 0xB0: // Delta Temperature
		return 3;
	case 0xC0: // Time
		return 4;
	case 0xD0: // Delta Heart Rate
		return 5;
	case 0xE0: // Alarms
		return 6;
	case 0xF0:
		switch (value & 0xFF) {
		case 0xF0: // More Alarms
			return 7;
		case 0xF1: // Absolute Depth
			return 8;
		case 0xF2: // Absolute RBT
			return 9;
		case 0xF3: // Absolute Temperature
			return 10;
		case 0xF4: // Absolute Pressure T1
			return 11;
		case 0xF5: // Absolute Pressure T2
			return 12;
		case 0xF6: // Absolute Pressure T3
			return 13;
		case 0xF7: // Absolute Heart Rate
			return 14;
		case 0xF8: // Compass Bearing
			return 15;
		case 0xF9: // Even More Alarms
			return 16;
		}
		break;
	}

	assert (0);

	return (unsigned int) -1;
}


typedef enum {
	DELTA_TANK_PRESSURE_DEPTH,
	DELTA_RBT,
	DELTA_TEMPERATURE,
	DELTA_TANK_PRESSURE,
	DELTA_DEPTH,
	DELTA_HEARTRATE,
	BEARING,
	ALARMS,
	TIME,
	ABSOLUTE_DEPTH,
	ABSOLUTE_TEMPERATURE,
	ABSOLUTE_TANK_1_PRESSURE,
	ABSOLUTE_TANK_2_PRESSURE,
	ABSOLUTE_TANK_D_PRESSURE,
	ABSOLUTE_RBT,
	ABSOLUTE_HEARTRATE
} uwatec_galileo_sample_t;

typedef struct uwatec_galileo_sample_info_t {
	uwatec_galileo_sample_t type;
	unsigned int nbits;
	unsigned int extrabytes;
} uwatec_galileo_sample_info_t;

static const
uwatec_galileo_sample_info_t uwatec_galileo_sol_table [] = {
	{DELTA_DEPTH,				7, 0}, // 0ddd dddd
	{DELTA_RBT,					5, 0}, // 100d dddd
	{DELTA_TANK_PRESSURE,		4, 0}, // 1010 dddd
	{DELTA_TEMPERATURE,			4, 0}, // 1011 dddd
	{TIME,						4, 0}, // 1100 dddd
	{DELTA_HEARTRATE,			4, 0}, // 1101 dddd
	{ALARMS,					4, 0}, // 1110 dddd
	{ALARMS,					0, 1}, // 1111 0000 dddddddd
	{ABSOLUTE_DEPTH,			0, 2}, // 1111 0001 dddddddd dddddddd
	{ABSOLUTE_RBT,				0, 1}, // 1111 0010 dddddddd
	{ABSOLUTE_TEMPERATURE,		0, 2}, // 1111 0011 dddddddd dddddddd
	{ABSOLUTE_TANK_1_PRESSURE,	0, 2}, // 1111 0100 dddddddd dddddddd
	{ABSOLUTE_TANK_2_PRESSURE,	0, 2}, // 1111 0101 dddddddd dddddddd
	{ABSOLUTE_TANK_D_PRESSURE,	0, 2}, // 1111 0110 dddddddd dddddddd
	{ABSOLUTE_HEARTRATE,		0, 1}, // 1111 0111 dddddddd
	{BEARING,					0, 2}, // 1111 1000 dddddddd dddddddd
	{ALARMS,					0, 1}, // 1111 1001 dddddddd
};

static parser_status_t
uwatec_galileo_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	uwatec_galileo_parser_t *parser = (uwatec_galileo_parser_t*) abstract;

	if (! parser_is_uwatec_galileo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	const uwatec_galileo_sample_info_t *table = NULL;
	unsigned int entries = 0;
	unsigned int header = 0;

	// Load the correct table.
	switch (parser->model) {
	case 0x11: // Galileo Sol
		header = 152;
		table = uwatec_galileo_sol_table;
		entries = NELEMENTS (uwatec_galileo_sol_table);
		break;
	default:
		return PARSER_STATUS_ERROR;
	}

	int complete = 1;
	int calibrated = 0;

	unsigned int time = 0;
	unsigned int rbt = 0;
	unsigned int tank = 0;
	double depth = 0, depth_calibration = 0;
	double temperature = 0;
	double pressure = 0;
	unsigned int heartrate = 0;
	unsigned char alarms = 0;

	unsigned int offset = header;
	while (offset < size) {
		parser_sample_value_t sample = {0};

		// Process the type bits in the bitstream.
		unsigned int id = uwatec_galileo_identify (data + offset, size - offset);
		assert (id < entries);

		// Process the remaining data bytes.
		unsigned int nbits = table[id].nbits;
		unsigned int n = NBITS - nbits;
		unsigned int value = data[offset] & (0xFF >> n);
		assert (offset + table[id].extrabytes + 1 <= size);
		for (unsigned int i = 0; i < table[id].extrabytes; ++i) {
			nbits += NBITS;
			value <<= NBITS;
			value += data[offset + i + 1];
		}

		// Skip the processed data bytes.
		offset += table[id].extrabytes + 1;

		// Fix the sign bit.
		signed int svalue = uwatec_galileo_fixsignbit (value, nbits);

		if (complete && table[id].type != TIME) {
			complete = 0;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);
		}

		// Parse the value.
		switch (table[id].type) {
		case DELTA_TANK_PRESSURE_DEPTH:
			pressure += ((signed char) ((svalue >> NBITS) & 0xFF)) / 4.0;
			depth += ((signed char) (svalue & 0xFF)) / 50.0;
			sample.pressure.tank = tank;
			sample.pressure.value = pressure;
			if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			sample.depth = depth - depth_calibration;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
			complete = 1;
			time += 4;
			break;
		case DELTA_RBT:
			rbt += svalue;
			sample.rbt = rbt;
			if (callback) callback (SAMPLE_TYPE_RBT, sample, userdata);
			break;
		case DELTA_TEMPERATURE:
			temperature += svalue / 2.5;
			sample.temperature = temperature;
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
			break;
		case DELTA_TANK_PRESSURE:
			pressure += svalue / 4.0;
			sample.pressure.tank = tank;
			sample.pressure.value = pressure;
			if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			break;
		case DELTA_DEPTH:
			depth += svalue / 50.0;
			sample.depth = depth - depth_calibration;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
			complete = 1;
			time += 4;
			break;
		case DELTA_HEARTRATE:
			heartrate += svalue;
			sample.heartbeat = heartrate;
			if (callback) callback (SAMPLE_TYPE_HEARTBEAT, sample, userdata);
			break;
		case BEARING:
			sample.bearing = value;
			if (callback) callback (SAMPLE_TYPE_BEARING, sample, userdata);
			break;
		case ALARMS:
			alarms = value;
			sample.vendor.type = SAMPLE_VENDOR_UWATEC_GALILEO;
			sample.vendor.size = sizeof (alarms);
			sample.vendor.data = &alarms;
			if (callback) callback (SAMPLE_TYPE_VENDOR, sample, userdata);
			break;
		case TIME:
			complete = 1;
			time += value * 4;
			break;
		case ABSOLUTE_DEPTH:
			depth = value / 50.0;
			if (!calibrated) {
				calibrated = 1;
				depth_calibration = depth;
			}
			sample.depth = depth - depth_calibration;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
			complete = 1;
			time += 4;
			break;
		case ABSOLUTE_TEMPERATURE:
			temperature = value / 2.5;
			sample.temperature = temperature;
			if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);
			break;
		case ABSOLUTE_TANK_D_PRESSURE:
			tank = 2;
			pressure = value / 4.0;
			sample.pressure.tank = tank;
			sample.pressure.value = pressure;
			if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			break;
		case ABSOLUTE_TANK_2_PRESSURE:
			tank = 1;
			pressure = value / 4.0;
			sample.pressure.tank = tank;
			sample.pressure.value = pressure;
			if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			break;
		case ABSOLUTE_TANK_1_PRESSURE:
			tank = 0;
			pressure = value / 4.0;
			sample.pressure.tank = tank;
			sample.pressure.value = pressure;
			if (callback) callback (SAMPLE_TYPE_PRESSURE, sample, userdata);
			break;
		case ABSOLUTE_RBT:
			rbt = value;
			sample.rbt = rbt;
			if (callback) callback (SAMPLE_TYPE_RBT, sample, userdata);
			break;
		case ABSOLUTE_HEARTRATE:
			heartrate = value;
			sample.heartbeat = heartrate;
			if (callback) callback (SAMPLE_TYPE_HEARTBEAT, sample, userdata);
			break;
		default:
			WARNING ("Unknown sample type.");
			break;
		}
	}

	assert (offset == size);

	return PARSER_STATUS_SUCCESS;
}
