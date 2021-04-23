/*
 * libdivecomputer
 *
 * Copyright (C) 2020 Linus Torvalds
 * Copyright (C) 2022 Jef Driesen
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "oceans_s1.h"
#include "oceans_s1_common.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"
#include "array.h"

#define SCUBA 0
#define APNEA 1

#define EVENT_DIVE_STARTED  0x0001
#define EVENT_DIVE_ENDED    0x0002
#define EVENT_DIVE_RESUMED  0x0004
#define EVENT_PING_SENT     0x0008
#define EVENT_PING_RECEIVED 0x0010
#define EVENT_DECO_STOP     0x0020
#define EVENT_SAFETY_STOP   0x0040
#define EVENT_BATTERY_LOW   0x0080
#define EVENT_BACKLIGHT_ON  0x0100

typedef struct oceans_s1_parser_t oceans_s1_parser_t;

struct oceans_s1_parser_t {
	dc_parser_t base;
	// Cached fields.
	dc_ticks_t timestamp;
	unsigned int cached;
	unsigned int number;
	unsigned int divemode;
	unsigned int oxygen;
	unsigned int maxdepth;
	unsigned int divetime;
};

static dc_status_t oceans_s1_parser_get_datetime(dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t oceans_s1_parser_get_field(dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t oceans_s1_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t oceans_s1_parser_vtable = {
	sizeof(oceans_s1_parser_t),
	DC_FAMILY_OCEANS_S1,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	oceans_s1_parser_get_datetime, /* datetime */
	oceans_s1_parser_get_field, /* fields */
	oceans_s1_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
oceans_s1_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	oceans_s1_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (oceans_s1_parser_t *) dc_parser_allocate (context, &oceans_s1_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->timestamp = 0;
	parser->number = 0;
	parser->divemode = 0;
	parser->oxygen = 0;
	parser->maxdepth = 0;
	parser->divetime = 0;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	oceans_s1_parser_t *parser = (oceans_s1_parser_t *) abstract;

	if (!parser->cached) {
		dc_status_t status = oceans_s1_parser_samples_foreach (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	if (!dc_datetime_gmtime (datetime, parser->timestamp))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	oceans_s1_parser_t *parser = (oceans_s1_parser_t *) abstract;

	if (!parser->cached) {
		dc_status_t status = oceans_s1_parser_samples_foreach (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->divemode == SCUBA;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->helium = 0.0;
			gasmix->oxygen = parser->oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->divemode) {
			case SCUBA:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case APNEA:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_parser_t *parser = (oceans_s1_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	size_t size = abstract->size;

	dc_ticks_t timestamp = 0;
	unsigned int number = 0, divemode = 0, oxygen = 0;
	unsigned int maxdepth = 0, divetime = 0;
	unsigned int interval = 10;
	unsigned int time = 0;

	char *ptr = NULL;
	size_t len = 0;
	int n = 0;
	while ((n = oceans_s1_getline (&ptr, &len, &data, &size)) != -1) {
		dc_sample_value_t sample = {0};

		// Ignore empty lines.
		if (n == 0)
			continue;

		// Ignore leading whitespace.
		const char *line = ptr;
		while (*line == ' ')
			line++;

		if (strncmp (line, "divelog", 7) == 0) {
			if (sscanf (line, "divelog v1,%us/sample", &interval) != 1) {
				ERROR (parser->base.context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}
			if (interval == 0) {
				ERROR (parser->base.context, "Invalid sample interval (%u).", interval);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}
		} else if (strncmp (line, "dive", 4) == 0) {
			if (sscanf (line, "dive %u,%u,%u," DC_FORMAT_INT64, &number, &divemode, &oxygen, &timestamp) != 4) {
				ERROR (parser->base.context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}
		} else if (strncmp (line, "continue", 8) == 0) {
			unsigned int depth = 0, seconds = 0;
			if (sscanf (line, "continue %u,%u", &depth, &seconds) != 2) {
				ERROR (parser->base.context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}

			// Create surface samples for the surface time,
			// and then a depth sample at the stated depth.
			unsigned int nsamples = seconds / interval;
			for (unsigned int i = 0; i < nsamples; ++i) {
				time += interval;
				sample.time = time * 1000;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				sample.depth = 0;
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
			}

			time += interval;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			sample.depth = depth / 100.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
		} else if (strncmp(line, "enddive", 7) == 0) {
			if (sscanf(line, "enddive %u,%u", &maxdepth, &divetime) != 2) {
				ERROR (parser->base.context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}
		} else if (strncmp (line, "endlog", 6) == 0) {
			// Nothing to do.
		} else {
			unsigned int depth = 0, events = 0;
			int temperature = 0;
			if (sscanf (line, "%u,%d,%u", &depth, &temperature, &events) != 3) {
				ERROR (parser->base.context, "Failed to parse the line '%s'.", line);
				status = DC_STATUS_DATAFORMAT;
				goto error_free;
			}

			time += interval;
			sample.time = time * 1000;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			sample.depth = depth / 100.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			sample.temperature = temperature;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

			if (events & EVENT_DECO_STOP) {
				sample.deco.type = DC_DECO_DECOSTOP;
			} else if (events & EVENT_SAFETY_STOP) {
				sample.deco.type = DC_DECO_SAFETYSTOP;
			} else {
				sample.deco.type = DC_DECO_NDL;
			}
			sample.deco.depth = 0.0;
			sample.deco.time = 0;
			sample.deco.tts = 0;
			if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
		}
	}

	// Cache the data for later use.
	parser->timestamp = timestamp;
	parser->number = number;
	parser->divemode = divemode;
	parser->oxygen = oxygen;
	parser->maxdepth = maxdepth;
	parser->divetime = divetime;
	parser->cached = 1;

error_free:
	free (ptr);
	return status;
}
