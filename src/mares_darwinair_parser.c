/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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
#include <string.h>

#include "mares_darwinair.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

#define HEADERSIZE 60

typedef struct mares_darwinair_parser_t mares_darwinair_parser_t;

struct mares_darwinair_parser_t {
	parser_t base;
};

static parser_status_t mares_darwinair_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t mares_darwinair_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime);
static parser_status_t mares_darwinair_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value);
static parser_status_t mares_darwinair_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t mares_darwinair_parser_destroy (parser_t *abstract);

static const parser_backend_t mares_darwinair_parser_backend = {
	PARSER_TYPE_MARES_DARWINAIR,
	mares_darwinair_parser_set_data, /* set_data */
	mares_darwinair_parser_get_datetime, /* datetime */
	mares_darwinair_parser_get_field, /* fields */
	mares_darwinair_parser_samples_foreach, /* samples_foreach */
	mares_darwinair_parser_destroy /* destroy */
};


static int
parser_is_mares_darwinair (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_darwinair_parser_backend;
}


parser_status_t
mares_darwinair_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	mares_darwinair_parser_t *parser = (mares_darwinair_parser_t *) malloc (sizeof (mares_darwinair_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &mares_darwinair_parser_backend);

	*out = (parser_t *) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_darwinair_parser_destroy (parser_t *abstract)
{
	if (! parser_is_mares_darwinair (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_darwinair_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_darwinair_parser_get_datetime (parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < HEADERSIZE)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year   = array_uint16_be (p);
		datetime->month  = p[2];
		datetime->day    = p[3];
		datetime->hour   = p[4];
		datetime->minute = p[5];
		datetime->second = 0;
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_darwinair_parser_get_field (parser_t *abstract, parser_field_type_t type, unsigned int flags, void *value)
{
	if (abstract->size < HEADERSIZE)
		return PARSER_STATUS_ERROR;

	const unsigned char *p = abstract->data;

	gasmix_t *gasmix = (gasmix_t *) value;

	if (value) {
		switch (type) {
		case FIELD_TYPE_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (p + 0x06) * 20;
			break;
		case FIELD_TYPE_MAXDEPTH:
			*((double *) value) = array_uint16_be (p + 0x08) / 10.0;
			break;
		case FIELD_TYPE_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case FIELD_TYPE_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = 0.21;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return PARSER_STATUS_UNSUPPORTED;
		}
	}

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_darwinair_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (abstract->size < HEADERSIZE)
		return PARSER_STATUS_ERROR;

	unsigned int time = 0;

	unsigned int offset = HEADERSIZE;
	while (offset + 3 <= abstract->size) {
			parser_sample_value_t sample = {0};

			// Surface Time (seconds).
			time += 20;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			// Depth (1/10 m).
			unsigned int depth = array_uint16_le (abstract->data + offset) & 0x07FF;
			sample.depth = depth / 10.0;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			offset += 3;
	}

	return PARSER_STATUS_SUCCESS;
}
