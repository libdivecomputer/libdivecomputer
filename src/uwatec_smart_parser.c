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

typedef struct uwatec_smart_parser_t uwatec_smart_parser_t;

struct uwatec_smart_parser_t {
	parser_t base;
	unsigned int model;
};

static parser_status_t uwatec_smart_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t uwatec_smart_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t uwatec_smart_parser_destroy (parser_t *abstract);

static const parser_backend_t uwatec_smart_parser_backend = {
	PARSER_TYPE_UWATEC_SMART,
	uwatec_smart_parser_set_data, /* set_data */
	uwatec_smart_parser_samples_foreach, /* samples_foreach */
	uwatec_smart_parser_destroy /* destroy */
};


static int
parser_is_uwatec_smart (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_smart_parser_backend;
}


parser_status_t
uwatec_smart_parser_create (parser_t **out, unsigned int model)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	uwatec_smart_parser_t *parser = malloc (sizeof (uwatec_smart_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &uwatec_smart_parser_backend);

	// Set the default values.
	parser->model = model;

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
uwatec_smart_parser_destroy (parser_t *abstract)
{
	if (! parser_is_uwatec_smart (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.	
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
uwatec_smart_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_uwatec_smart (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static unsigned int
uwatec_smart_identify (const unsigned char data[], unsigned int size)
{
	unsigned int count = 0;
	for (unsigned int i = 0; i < size; ++i) {
		unsigned char value = data[i];
		for (unsigned int j = 0; j < NBITS; ++j) {
			unsigned char mask = 1 << (NBITS - 1 - j);
			if ((value & mask) == 0)
				return count;
			count++;
		}
	}

	assert (0);

	return (unsigned int) -1;
}


static unsigned int
uwatec_smart_fixsignbit (unsigned int x, unsigned int n)
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


typedef enum {
	DELTA_TANK_PRESSURE_DEPTH,
	DELTA_RBT,
	DELTA_TEMPERATURE,
	DELTA_TANK_PRESSURE,
	DELTA_DEPTH,
	ALARMS,
	TIME,
	ABSOLUTE_DEPTH,
	ABSOLUTE_TEMPERATURE,
	ABSOLUTE_TANK_1_PRESSURE,
	ABSOLUTE_TANK_2_PRESSURE,
	ABSOLUTE_TANK_D_PRESSURE,
	ABSOLUTE_RBT
} uwatec_smart_sample_t;

typedef struct uwatec_smart_sample_info_t {
	uwatec_smart_sample_t type;
	unsigned int ntypebits;
	unsigned int ignoretype;
	unsigned int extrabytes;
} uwatec_smart_sample_info_t;

static const
uwatec_smart_sample_info_t uwatec_smart_pro_table [] = {
	{DELTA_DEPTH, 				1, 0, 0}, // 0ddddddd
	{DELTA_TEMPERATURE, 		2, 0, 0}, // 10dddddd
	{TIME, 						3, 0, 0}, // 110ddddd
	{ALARMS, 					4, 0, 0}, // 1110dddd
	{DELTA_DEPTH, 				5, 0, 1}, // 11110ddd dddddddd
	{DELTA_TEMPERATURE, 		6, 0, 1}, // 111110dd dddddddd
	{ABSOLUTE_DEPTH, 			7, 1, 2}, // 1111110d dddddddd dddddddd
	{ABSOLUTE_TEMPERATURE, 		8, 0, 2}, // 11111110 dddddddd dddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_aladin_table [] = {
	{DELTA_DEPTH, 				1, 0, 0}, // 0ddddddd
	{DELTA_TEMPERATURE, 		2, 0, 0}, // 10dddddd
	{TIME, 						3, 0, 0}, // 110ddddd
	{ALARMS, 					4, 0, 0}, // 1110dddd
	{DELTA_DEPTH, 				5, 0, 1}, // 11110ddd dddddddd
	{DELTA_TEMPERATURE, 		6, 0, 1}, // 111110dd dddddddd
	{ABSOLUTE_DEPTH, 			7, 1, 2}, // 1111110d dddddddd dddddddd
	{ABSOLUTE_TEMPERATURE, 		8, 0, 2}, // 11111110 dddddddd dddddddd
	{ALARMS, 					9, 0, 0}, // 11111111 0ddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_com_table [] = {
	{DELTA_TANK_PRESSURE_DEPTH,  1, 0, 1}, // 0ddddddd dddddddd
	{DELTA_RBT, 				 2, 0, 0}, // 10dddddd
	{DELTA_TEMPERATURE, 		 3, 0, 0}, // 110ddddd
	{DELTA_TANK_PRESSURE, 		 4, 0, 1}, // 1110dddd dddddddd
	{DELTA_DEPTH, 				 5, 0, 1}, // 11110ddd dddddddd
	{DELTA_TEMPERATURE, 		 6, 0, 1}, // 111110dd dddddddd
	{ALARMS, 					 7, 1, 1}, // 1111110d dddddddd
	{TIME, 						 8, 0, 1}, // 11111110 dddddddd
	{ABSOLUTE_DEPTH, 			 9, 1, 2}, // 11111111 0ddddddd dddddddd dddddddd
	{ABSOLUTE_TANK_1_PRESSURE, 	10, 1, 2}, // 11111111 10dddddd dddddddd dddddddd
	{ABSOLUTE_TEMPERATURE, 		11, 1, 2}, // 11111111 110ddddd dddddddd dddddddd
	{ABSOLUTE_RBT, 				12, 1, 1}, // 11111111 1110dddd dddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_tec_table [] = {
	{DELTA_TANK_PRESSURE_DEPTH,	 1, 0, 1}, // 0ddddddd dddddddd
	{DELTA_RBT, 				 2, 0, 0}, // 10dddddd
	{DELTA_TEMPERATURE, 		 3, 0, 0}, // 110ddddd
	{DELTA_TANK_PRESSURE, 		 4, 0, 1}, // 1110dddd dddddddd
	{DELTA_DEPTH, 				 5, 0, 1}, // 11110ddd dddddddd
	{DELTA_TEMPERATURE, 		 6, 0, 1}, // 111110dd dddddddd
	{ALARMS, 					 7, 1, 1}, // 1111110d dddddddd
	{TIME, 						 8, 0, 1}, // 11111110 dddddddd
	{ABSOLUTE_DEPTH, 			 9, 1, 2}, // 11111111 0ddddddd dddddddd dddddddd
	{ABSOLUTE_TEMPERATURE, 		10, 1, 2}, // 11111111 10dddddd dddddddd dddddddd
	{ABSOLUTE_TANK_1_PRESSURE, 	11, 1, 2}, // 11111111 110ddddd dddddddd dddddddd
	{ABSOLUTE_TANK_2_PRESSURE, 	12, 1, 2}, // 11111111 1110dddd dddddddd dddddddd
	{ABSOLUTE_TANK_D_PRESSURE, 	13, 1, 2}, // 11111111 11110ddd dddddddd dddddddd
	{ABSOLUTE_RBT, 				14, 1, 1}, // 11111111 111110dd dddddddd
};


static parser_status_t
uwatec_smart_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	uwatec_smart_parser_t *parser = (uwatec_smart_parser_t*) abstract;

	if (! parser_is_uwatec_smart (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	const uwatec_smart_sample_info_t *table = NULL;
	unsigned int entries = 0;
	unsigned int header = 0;

	// Load the correct table.
	switch (parser->model) {
	case 0x10: // Smart Pro
		header = 92;
		table = uwatec_smart_pro_table;
		entries = NELEMENTS (uwatec_smart_pro_table);
		break;
	case 0x12: // Aladin Tec, Prime
		header = 108;
		table = uwatec_smart_aladin_table;
		entries = NELEMENTS (uwatec_smart_aladin_table);
		break;
	case 0x14: // Smart Com
		header = 100;
		table = uwatec_smart_com_table;
		entries = NELEMENTS (uwatec_smart_com_table);
		break;
	case 0x18: // Smart Tec
	case 0x1C: // Smart Z
		header = 132;
		table = uwatec_smart_tec_table;
		entries = NELEMENTS (uwatec_smart_tec_table);
		break;
	default:
		return PARSER_STATUS_ERROR;
	}

	int complete = 1;
	int calibrated = 0;

	unsigned int time = 0;
	unsigned int rbt = 99;
	unsigned int tank = 0;
	double depth = 0, depth_calibration = 0;
	double temperature = 0;
	double pressure = 0;
	unsigned char alarms = 0;

	unsigned int offset = header;
	while (offset < size) {
		parser_sample_value_t sample = {0};

		// Count the number of type bits in the bitstream.
		unsigned int id = uwatec_smart_identify (data + offset, size - offset);
		assert (id < entries);	

		// Skip the processed type bytes.
		offset += table[id].ntypebits / NBITS;

		// Process the remaining data bits.
		unsigned int nbits = 0;
		unsigned int value = 0;
		unsigned int n = table[id].ntypebits % NBITS;
		if (n > 0) {
			nbits = NBITS - n;
			value = data[offset] & (0xFF >> n);
			if (table[id].ignoretype) {
				// Ignore any data bits that are stored in
				// the last type byte for certain samples.
				nbits = 0;
				value = 0;
			}
			offset++;
		}

		// Process the extra data bytes.
		assert (offset + table[id].extrabytes <= size);
		for (unsigned int i = 0; i < table[id].extrabytes; ++i) {
			nbits += NBITS;
			value <<= NBITS;
			value += data[offset];
			offset++;
		}

		// Fix the sign bit.
		signed int svalue = uwatec_smart_fixsignbit (value, nbits);

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
		case ALARMS:
			alarms = value;
			sample.vendor.type = SAMPLE_VENDOR_UWATEC_SMART;
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
		default:
			WARNING ("Unknown sample type.");
			break;
		}
	}

	assert (offset == size);

	return PARSER_STATUS_SUCCESS;
}
