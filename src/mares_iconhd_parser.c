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

#include <stdlib.h>

#include "mares_iconhd.h"
#include "parser-private.h"
#include "utils.h"
#include "array.h"

typedef struct mares_iconhd_parser_t mares_iconhd_parser_t;

struct mares_iconhd_parser_t {
	parser_t base;
};

static parser_status_t mares_iconhd_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t mares_iconhd_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t mares_iconhd_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t mares_iconhd_parser_destroy (parser_t *abstract);

static const parser_backend_t mares_iconhd_parser_backend = {
	PARSER_TYPE_MARES_ICONHD,
	mares_iconhd_parser_set_data, /* set_data */
	mares_iconhd_parser_get_datetime, /* datetime */
	mares_iconhd_parser_samples_foreach, /* samples_foreach */
	mares_iconhd_parser_destroy /* destroy */
};


static int
parser_is_mares_iconhd (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_iconhd_parser_backend;
}


parser_status_t
mares_iconhd_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) malloc (sizeof (mares_iconhd_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &mares_iconhd_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_iconhd_parser_destroy (parser_t *abstract)
{
	if (! parser_is_mares_iconhd (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_iconhd_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_iconhd_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 4)
		return PARSER_STATUS_ERROR;

	unsigned int length = array_uint32_le (abstract->data);

	if (abstract->size < length || length < 0x60)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data + length - 0x56;

	if (datetime) {
		datetime->hour   = array_uint16_le (p + 0);
		datetime->minute = array_uint16_le (p + 2);
		datetime->second = 0;
		datetime->day    = array_uint16_le (p + 4);
		datetime->month  = array_uint16_le (p + 6) + 1;
		datetime->year   = array_uint16_le (p + 8) + 1900;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_iconhd_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (abstract->size < 4)
		return PARSER_STATUS_ERROR;

	unsigned int length = array_uint32_le (abstract->data);

	if (abstract->size < length || length < 0x60)
		return PARSER_STATUS_ERROR;

	const unsigned char *data = abstract->data;
	unsigned int size = length - 0x60;

	unsigned int time = 0;
	unsigned int interval = 5;

	unsigned int offset = 0;
	while (offset + 8 <= size) {
		parser_sample_value_t sample = {0};

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_le (data + offset + 4);
		sample.depth = depth / 10.0;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

		// Temperature (1/10 °C).
		unsigned int temperature = array_uint16_le (data + offset + 6);
		sample.temperature = temperature / 10.0;
		if (callback) callback (SAMPLE_TYPE_TEMPERATURE, sample, userdata);

		offset += 8;
	}

	return PARSER_STATUS_SUCCESS;
}
