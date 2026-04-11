/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Kalen Josifovski
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

#include "crest_cr5l.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define DETAIL_OFFSET 0x224

#define HEADER_DATE       0x08
#define HEADER_DURATION   0x32
#define HEADER_START_TS   0x38
#define HEADER_MAXDEPTH   0x50
#define HEADER_AVGDEPTH   0x52
#define HEADER_MINTEMP    0x58

typedef struct crest_cr5l_parser_t {
	dc_parser_t base;
} crest_cr5l_parser_t;

static dc_status_t crest_cr5l_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t crest_cr5l_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t crest_cr5l_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t crest_cr5l_parser_vtable = {
	sizeof(crest_cr5l_parser_t),
	DC_FAMILY_CREST_CR5L,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	crest_cr5l_parser_get_datetime, /* datetime */
	crest_cr5l_parser_get_field, /* fields */
	crest_cr5l_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static int
crest_cr5l_is_temp_word (unsigned int value)
{
	return value >= 150 && value <= 400;
}

static int
crest_cr5l_is_date_ascii (const unsigned char *data, size_t size)
{
	if (size < HEADER_DATE + 8)
		return 0;

	for (unsigned int i = 0; i < 8; ++i) {
		if (data[HEADER_DATE + i] < '0' || data[HEADER_DATE + i] > '9')
			return 0;
	}

	return 1;
}

static int
crest_cr5l_valid_0x51_at (const unsigned char *data, unsigned int size, unsigned int offset)
{
	if (offset + 6 > size || data[offset] != 0x51)
		return 0;

	unsigned int depth = array_uint16_le (data + offset + 2);
	unsigned int temperature = array_uint16_le (data + offset + 4);
	unsigned int checksum = data[offset + 1];
	unsigned int csum = (checksum +
		data[offset + 2] + data[offset + 3] +
		data[offset + 4] + data[offset + 5]) & 0xFF;

	if (depth > 20000 || !crest_cr5l_is_temp_word (temperature))
		return 0;

	return csum == 0xFF;
}

static unsigned int
crest_cr5l_skip_0x58_block (const unsigned char *data, unsigned int size, unsigned int offset)
{
	if (offset + 8 > size || data[offset] != 0x58)
		return 0;

	for (unsigned int end = offset + 8; end + 5 <= size && end < offset + 256; ++end) {
		if (!crest_cr5l_valid_0x51_at (data, size, end))
			continue;

		if (((end - offset - 4) % 2) != 0)
			continue;

		unsigned int nvalues = (end - offset - 4) / 2;
		if (nvalues == 0)
			continue;

		int ok = 1;
		for (unsigned int i = offset + 4; i < end; i += 2) {
			unsigned int value = array_uint16_le (data + i);
			if (value > 5000) {
				ok = 0;
				break;
			}
		}

		if (ok)
			return end;
	}

	return 0;
}

dc_status_t
crest_cr5l_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	crest_cr5l_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (size < DETAIL_OFFSET || !crest_cr5l_is_date_ascii (data, size))
		return DC_STATUS_DATAFORMAT;

	parser = (crest_cr5l_parser_t *) dc_parser_allocate (context, &crest_cr5l_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADER_START_TS + 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (data + HEADER_START_TS);
	if (!dc_datetime_gmtime (datetime, timestamp))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	(void) flags;

	if (size < HEADER_MINTEMP + 2)
		return DC_STATUS_DATAFORMAT;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + HEADER_DURATION);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + HEADER_MAXDEPTH) / 100.0;
			break;
		case DC_FIELD_AVGDEPTH:
			*((double *) value) = array_uint16_le (data + HEADER_AVGDEPTH) / 100.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = array_uint16_le (data + HEADER_MINTEMP) / 10.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
crest_cr5l_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < DETAIL_OFFSET + 6)
		return DC_STATUS_DATAFORMAT;

	unsigned int duration = array_uint16_le (data + HEADER_DURATION);
	const unsigned char *detail = data + DETAIL_OFFSET;
	unsigned int detail_size = size - DETAIL_OFFSET;

	unsigned int nsamples = 0;
	unsigned int offset = 0;
	while (offset + 6 <= detail_size) {
		if (crest_cr5l_valid_0x51_at (detail, detail_size, offset)) {
			nsamples++;
			offset += 6;
			continue;
		}

		if (detail[offset] == 0x58) {
			unsigned int next = crest_cr5l_skip_0x58_block (detail, detail_size, offset);
			if (next != 0) {
				offset = next;
				continue;
			}
		}

		offset++;
	}

	if (nsamples == 0)
		return DC_STATUS_DATAFORMAT;

	offset = 0;
	unsigned int index = 0;
	while (offset + 6 <= detail_size) {
		if (crest_cr5l_valid_0x51_at (detail, detail_size, offset)) {
			dc_sample_value_t sample = {0};
			unsigned int time_ms = (unsigned int) ((((unsigned long long) duration) * 1000ULL * index) / nsamples);
			unsigned int depth_cm = array_uint16_le (detail + offset + 2);
			unsigned int temperature = array_uint16_le (detail + offset + 4);

			sample.time = time_ms;
			if (callback)
				callback (DC_SAMPLE_TIME, &sample, userdata);

			sample.depth = depth_cm / 100.0;
			if (callback)
				callback (DC_SAMPLE_DEPTH, &sample, userdata);

			sample.temperature = temperature / 10.0;
			if (callback)
				callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

			index++;
			offset += 6;
			continue;
		}

		if (detail[offset] == 0x58) {
			unsigned int next = crest_cr5l_skip_0x58_block (detail, detail_size, offset);
			if (next != 0) {
				offset = next;
				continue;
			}
		}

		offset++;
	}

	return DC_STATUS_SUCCESS;
}
