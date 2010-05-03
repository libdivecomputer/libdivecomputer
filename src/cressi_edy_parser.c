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
#include <assert.h>

#include "cressi_edy.h"
#include "parser-private.h"
#include "utils.h"
#include "array.h"

typedef struct cressi_edy_parser_t cressi_edy_parser_t;

struct cressi_edy_parser_t {
	parser_t base;
};

static parser_status_t cressi_edy_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t cressi_edy_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t cressi_edy_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t cressi_edy_parser_destroy (parser_t *abstract);

static const parser_backend_t cressi_edy_parser_backend = {
	PARSER_TYPE_CRESSI_EDY,
	cressi_edy_parser_set_data, /* set_data */
	cressi_edy_parser_get_datetime, /* datetime */
	cressi_edy_parser_samples_foreach, /* samples_foreach */
	cressi_edy_parser_destroy /* destroy */
};


static int
parser_is_cressi_edy (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &cressi_edy_parser_backend;
}


parser_status_t
cressi_edy_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	cressi_edy_parser_t *parser = (cressi_edy_parser_t *) malloc (sizeof (cressi_edy_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &cressi_edy_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
cressi_edy_parser_destroy (parser_t *abstract)
{
	if (! parser_is_cressi_edy (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
cressi_edy_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_cressi_edy (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
cressi_edy_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 32)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year = bcd2dec (p[4]) + 2000;
		datetime->month = (p[5] & 0xF0) >> 4;
		datetime->day = (p[5] & 0x0F) * 10 + ((p[6] & 0xF0) >> 4);
		datetime->hour = bcd2dec (p[14]);
		datetime->minute = bcd2dec (p[15]);
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
cressi_edy_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = 1;

	unsigned int offset = 32;
	while (offset + 2 <= size) {
		parser_sample_value_t sample = {0};

		if (data[offset] == 0xFF)
			break;

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = bcd2dec (data[offset + 0]) * 100 + bcd2dec (data[offset + 1]);
		sample.depth = depth / 10.0;
		if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

		offset += 2;
	}

	return PARSER_STATUS_SUCCESS;
}
