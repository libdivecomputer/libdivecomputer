/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Ryan Gardner, Jef Driesen
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <libdivecomputer/units.h>

#include "deepsix_excursion.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define HEADERSIZE_MIN 128

#define MAX_SAMPLES   7
#define MAX_EVENTS    7
#define MAX_GASMIXES  20

#define ALARM        0x0001
#define TEMPERATURE  0x0002
#define DECO         0x0003
#define CEILING      0x0004
#define CNS          0x0005

#define SAMPLE_TEMPERATURE 0
#define SAMPLE_DECO_NDL    1
#define SAMPLE_CNS         2

#define EVENT_CHANGE_GAS            7
#define EVENT_ALARMS                8
#define EVENT_CHANGE_SETPOINT       9
#define EVENT_SAMPLES_MISSED       10
#define EVENT_RESERVED             15

#define ALARM_ASCENTRATE  0
#define ALARM_CEILING     1
#define ALARM_PO2         2
#define ALARM_MAXDEPTH    3
#define ALARM_DIVETIME    4
#define ALARM_CNS         5

#define DECOSTOP   0x02
#define SAFETYSTOP 0x04

#define DENSITY 1024

#define UNDEFINED 0xFFFFFFFF

#define FWVERSION(major,minor) ( \
		((((major) + '0') & 0xFF) << 8) | \
		((minor) & 0xFF))

typedef struct deepsix_excursion_layout_t {
	unsigned int headersize;
	unsigned int version;
	unsigned int divemode;
	unsigned int samplerate;
	unsigned int salinity;
	unsigned int datetime;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int temperature_min;
	unsigned int avgdepth;
	unsigned int firmware;
	unsigned int temperature_surf;
	unsigned int atmospheric;
	unsigned int gf;
} deepsix_excursion_layout_t;

typedef struct deepsix_excursion_gasmix_t {
	unsigned int id;
	unsigned int oxygen;
	unsigned int helium;
} deepsix_excursion_gasmix_t;

typedef struct deepsix_excursion_sample_info_t {
	unsigned int type;
	unsigned int divisor;
	unsigned int size;
} deepsix_excursion_sample_info_t;

typedef struct deepsix_excursion_event_info_t {
	unsigned int type;
	unsigned int size;
} deepsix_excursion_event_info_t;

typedef struct deepsix_excursion_parser_t {
	dc_parser_t base;
	unsigned int cached;
	unsigned int ngasmixes;
	deepsix_excursion_gasmix_t gasmix[MAX_GASMIXES];
} deepsix_excursion_parser_t;

static dc_status_t deepsix_excursion_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t deepsix_excursion_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t deepsix_excursion_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t deepsix_excursion_parser_samples_foreach_v0 (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t deepsix_excursion_parser_samples_foreach_v1 (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t deepsix_parser_vtable = {
	sizeof(deepsix_excursion_parser_t),
	DC_FAMILY_DEEPSIX_EXCURSION,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	deepsix_excursion_parser_get_datetime, /* datetime */
	deepsix_excursion_parser_get_field, /* fields */
	deepsix_excursion_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const deepsix_excursion_layout_t deepsix_excursion_layout_v0 = {
	156,/* headersize */
	UNDEFINED, /* version */
	4,  /* divemode */
	24, /* samplerate */
	UNDEFINED, /* salinity */
	12, /* datetime */
	20, /* divetime */
	28, /* maxdepth */
	32, /* temperature_min */
	UNDEFINED, /* avgdepth */
	48, /* firmware */
	UNDEFINED, /* temperature_surf */
	56, /* atmospheric */
	UNDEFINED, /* gf */
};

static const deepsix_excursion_layout_t deepsix_excursion_layout_v1 = {
	129,/* headersize */
	3,  /* version */
	4,  /* divemode */
	5,  /* samplerate */
	7,  /* salinity */
	12, /* datetime */
	19, /* divetime */
	29, /* maxdepth */
	31, /* temperature_min */
	33, /* avgdepth */
	35, /* firmware */
	43, /* temperature_surf */
	45, /* atmospheric */
	127, /* gf */
};

static double
pressure_to_depth(unsigned int depth, unsigned int atmospheric, unsigned int density)
{
	return ((signed int)(depth - atmospheric)) * (BAR / 1000.0) / (density * GRAVITY);
}

static unsigned int
deepsix_excursion_find_gasmix(deepsix_excursion_parser_t *parser, unsigned int o2, unsigned int he, unsigned int id)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->gasmix[i].oxygen && he == parser->gasmix[i].helium && id == parser->gasmix[i].id)
			break;
		i++;
	}
	return i;
}

dc_status_t
deepsix_excursion_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	deepsix_excursion_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (deepsix_excursion_parser_t *) dc_parser_allocate (context, &deepsix_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->cached = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < MAX_GASMIXES; ++i) {
		parser->gasmix[i].id = 0;
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADERSIZE_MIN)
		return DC_STATUS_DATAFORMAT;

	unsigned int version = data[3];
	const deepsix_excursion_layout_t *layout = version == 0 ?
		&deepsix_excursion_layout_v0 : &deepsix_excursion_layout_v1;

	if (size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int firmware = array_uint16_be (data + layout->firmware + 4);

	const unsigned char *p = data + layout->datetime;

	if (datetime) {
		datetime->year   = p[0] + 2000;
		datetime->month  = p[1];
		datetime->day    = p[2];
		datetime->hour   = p[3];
		datetime->minute = p[4];
		datetime->second = p[5];

		if (version == 0) {
			if (firmware >= FWVERSION(5, 'B')) {
				datetime->timezone = (p[6] - 12) * 3600;
			} else {
				datetime->timezone = DC_TIMEZONE_NONE;
			}
		} else {
			datetime->timezone = ((signed char) p[6]) * 900;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	deepsix_excursion_parser_t *parser = (deepsix_excursion_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	if (size < HEADERSIZE_MIN)
		return DC_STATUS_DATAFORMAT;

	unsigned int version = data[3];
	const deepsix_excursion_layout_t *layout = version == 0 ?
		&deepsix_excursion_layout_v0 : &deepsix_excursion_layout_v1;

	if (size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	if (version != 0 && !parser->cached) {
		dc_status_t rc = deepsix_excursion_parser_samples_foreach_v1(abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	unsigned int atmospheric = array_uint16_le(data + layout->atmospheric);
	unsigned int density = DENSITY;
	if (layout->salinity != UNDEFINED) {
		density = 1000 + data[layout->salinity] * 10;
	}

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint32_le(data + layout->divetime);
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = pressure_to_depth(array_uint16_le(data + layout->maxdepth), atmospheric, density);
			break;
		case DC_FIELD_AVGDEPTH:
			if (layout->avgdepth == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = pressure_to_depth(array_uint16_le(data + layout->avgdepth), atmospheric, density);
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = DC_USAGE_NONE;
			gasmix->oxygen   = parser->gasmix[flags].oxygen / 100.0;
			gasmix->helium   = parser->gasmix[flags].helium / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed int) array_uint16_le(data + layout->temperature_min) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_SURFACE:
			if (layout->temperature_surf == UNDEFINED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = (signed int) array_uint16_le(data + layout->temperature_surf) / 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = atmospheric / 1000.0;
			break;
		case DC_FIELD_SALINITY:
			water->type	= (density == 1000) ? DC_WATER_FRESH : DC_WATER_SALT;
			water->density = density;
			break;
		case DC_FIELD_DIVEMODE:
			switch (data[layout->divemode]) {
			case 0:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case 1:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case 2:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			case 3:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_DECOMODEL:
			decomodel->type = DC_DECOMODEL_BUHLMANN;
			decomodel->conservatism = 0;
			if (layout->gf != UNDEFINED) {
				decomodel->params.gf.low  = data[layout->gf + 0];
				decomodel->params.gf.high = data[layout->gf + 1];
			} else {
				decomodel->params.gf.low  = 0;
				decomodel->params.gf.high = 0;
			}
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_samples_foreach_v0 (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	const deepsix_excursion_layout_t *layout = &deepsix_excursion_layout_v0;

	if (size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	int firmware4c = memcmp(data + layout->firmware, "D01-4C", 6) == 0;

	unsigned int maxtype = firmware4c ? TEMPERATURE : CNS;

	unsigned int interval = array_uint32_le(data + layout->samplerate);
	unsigned int atmospheric = array_uint32_le(data + layout->atmospheric);

	unsigned int time = 0;
	unsigned int offset = layout->headersize;
	while (offset + 1 < size) {
		dc_sample_value_t sample = {0};

		// Get the sample type.
		unsigned int type = data[offset];
		if (type < 1 || type > maxtype) {
			ERROR (abstract->context, "Unknown sample type (%u).", type);
			return DC_STATUS_DATAFORMAT;
		}

		// Get the sample length.
		unsigned int length = 1;
		if (type == ALARM || type == CEILING) {
			length = 8;
		} else if (type == TEMPERATURE || type == DECO || type == CNS) {
			length = 6;
		}

		// Verify the length.
		if (offset + length > size) {
			WARNING (abstract->context, "Unexpected end of data.");
			break;
		}

		unsigned int misc = data[offset + 1];
		unsigned int depth = array_uint16_le(data + offset + 2);

		if (type == TEMPERATURE) {
			time += interval;
			sample.time = time * 1000;
			if (callback) callback(DC_SAMPLE_TIME, &sample, userdata);

			sample.depth = pressure_to_depth(depth, atmospheric, DENSITY);
			if (callback) callback(DC_SAMPLE_DEPTH, &sample, userdata);
		}

		if (type == ALARM) {
			unsigned int alarm_time  = array_uint16_le(data + offset + 4);
			unsigned int alarm_value = array_uint16_le(data + offset + 6);
		} else if (type == TEMPERATURE) {
			unsigned int temperature = array_uint16_le(data + offset + 4);
			if (firmware4c) {
				if (temperature > 1300) {
					length = 8;
				} else if (temperature >= 10) {
					sample.temperature = temperature / 10.0;
					if (callback) callback(DC_SAMPLE_TEMPERATURE, &sample, userdata);
				}
			} else {
				sample.temperature = temperature / 10.0;
				if (callback) callback(DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}
		} else if (type == DECO) {
			unsigned int deco = array_uint16_le(data + offset + 4);
		} else if (type == CEILING) {
			unsigned int ceiling_depth = array_uint16_le(data + offset + 4);
			unsigned int ceiling_time  = array_uint16_le(data + offset + 6);
		} else if (type == CNS) {
			unsigned int cns = array_uint16_le(data + offset + 4);
			sample.cns = cns / 100.0;
			if (callback) callback(DC_SAMPLE_CNS, &sample, userdata);
		}

		offset += length;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_samples_foreach_v1 (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	deepsix_excursion_parser_t *parser = (deepsix_excursion_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	const deepsix_excursion_layout_t *layout = &deepsix_excursion_layout_v1;

	if (size < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int headersize = data[2];
	if (headersize < layout->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int samplerate = data[layout->samplerate];
	unsigned int atmospheric = array_uint16_le(data + layout->atmospheric);
	unsigned int density = 1000 + data[layout->salinity] * 10;

	unsigned int offset = headersize;
	if (offset + 1 > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int nconfig = data[offset];
	if (nconfig > MAX_SAMPLES) {
		ERROR(abstract->context, "Too many sample descriptors (%u).", nconfig);
		return DC_STATUS_DATAFORMAT;
	}

	offset += 1;

	if (offset + 3 * nconfig > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	deepsix_excursion_sample_info_t sample_info[MAX_SAMPLES] = {{0}};
	for (unsigned int i = 0; i < nconfig; i++) {
		sample_info[i].type    = data[offset + 3 * i + 0];
		sample_info[i].size    = data[offset + 3 * i + 1];
		sample_info[i].divisor = data[offset + 3 * i + 2];

		if (sample_info[i].divisor) {
			switch (sample_info[i].type) {
			case SAMPLE_CNS:
			case SAMPLE_TEMPERATURE:
				if (sample_info[i].size != 2) {
					ERROR(abstract->context, "Unexpected sample size (%u).", sample_info[i].size);
					return DC_STATUS_DATAFORMAT;
				}
				break;
			case SAMPLE_DECO_NDL:
				if (sample_info[i].size != 7) {
					ERROR(abstract->context, "Unexpected sample size (%u).", sample_info[i].size);
					return DC_STATUS_DATAFORMAT;
				}
				break;
			default:
				WARNING (abstract->context, "Unknown sample descriptor (%u %u %u).",
					sample_info[i].type, sample_info[i].size, sample_info[i].divisor);
				break;
			}
		}
	}

	offset += 3 * nconfig;

	if (offset + 1 > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int nevents = data[offset];
	if (nevents > MAX_EVENTS) {
		ERROR(abstract->context, "Too many event descriptors (%u).", nevents);
		return DC_STATUS_DATAFORMAT;
	}

	offset += 1;

	if (offset + 2 * nevents > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	deepsix_excursion_event_info_t event_info[MAX_EVENTS] = {{0}};
	for (unsigned int i = 0; i < nevents; i++) {
		event_info[i].type = data[offset + 2 * i];
		event_info[i].size = data[offset + 2 * i + 1];

		switch (event_info[i].type) {
		case EVENT_ALARMS:
			if (event_info[i].size != 1) {
				ERROR(abstract->context, "Unexpected event size (%u).", event_info[i].size);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case EVENT_CHANGE_GAS:
			if (event_info[i].size != 3) {
				ERROR(abstract->context, "Unexpected event size (%u).", event_info[i].size);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case EVENT_CHANGE_SETPOINT:
			if (event_info[i].size != 4 && event_info[i].size != 2) {
				ERROR(abstract->context, "Unexpected event size (%u).", event_info[i].size);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case EVENT_SAMPLES_MISSED:
			if (event_info[i].size != 6) {
				ERROR(abstract->context, "Unexpected event size (%u).", event_info[i].size);
				return DC_STATUS_DATAFORMAT;
			}
			break;
		default:
			WARNING (abstract->context, "Unknown event descriptor (%u %u).",
				event_info[i].type, event_info[i].size);
			break;
		}
	}

	offset += 2 * nevents;

	unsigned int time = 0;
	unsigned int nsamples = 0;
	while (offset + 3 <= size) {
		dc_sample_value_t sample = {0};
		nsamples++;

		// Time (seconds).
		time += samplerate;
		sample.time = time * 1000;
		if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = pressure_to_depth(depth, atmospheric, density);
		if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);
		offset += 2;

		// event info
		unsigned int length = data[offset];
		offset += 1;

		if (offset + length > size) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}

		if (length) {
			if (length < 2) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}

			unsigned int events = array_uint16_le (data + offset);
			unsigned int event_offset = 2;

			for (unsigned int i = 0; i < nevents; i++) {
				if ((events & (1 << event_info[i].type)) == 0)
					continue;

				if (event_offset + event_info[i].size > length) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int alarms = 0;
				unsigned int id = 0, o2 = 0, he = 0;
				unsigned int mix_idx = 0;
				unsigned int count = 0, timestamp = 0;
				switch (event_info[i].type) {
				case EVENT_ALARMS:
					alarms = data[offset + event_offset];
					for (unsigned int v = alarms, j = 0; v; v >>= 1, ++j) {
						if ((v & 1) == 0)
							continue;

						sample.event.type = SAMPLE_EVENT_NONE;
						sample.event.time = 0;
						sample.event.flags = 0;
						sample.event.value = 0;
						switch (j) {
						case ALARM_ASCENTRATE:
							sample.event.type = SAMPLE_EVENT_ASCENT;
							break;
						case ALARM_CEILING:
							sample.event.type = SAMPLE_EVENT_CEILING;
							break;
						case ALARM_PO2:
							sample.event.type = SAMPLE_EVENT_PO2;
							break;
						case ALARM_MAXDEPTH:
							sample.event.type = SAMPLE_EVENT_MAXDEPTH;
							break;
						case ALARM_DIVETIME:
							sample.event.type = SAMPLE_EVENT_DIVETIME;
							break;
						case ALARM_CNS:
							break;
						default:
							WARNING (abstract->context, "Unknown event (%u).", j);
							break;
						}
						if (sample.event.type != SAMPLE_EVENT_NONE) {
							if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
						}
					}
					break;
				case EVENT_CHANGE_GAS:
					id = data[offset + event_offset];
					o2 = data[offset + event_offset + 1];
					he = data[offset + event_offset + 2];

					mix_idx = deepsix_excursion_find_gasmix(parser, o2, he, id);
					if (mix_idx >= parser->ngasmixes) {
						if (mix_idx >= MAX_GASMIXES) {
							ERROR (abstract->context, "Maximum number of gas mixes reached.");
							return DC_STATUS_NOMEMORY;
						}
						parser->gasmix[mix_idx].oxygen = o2;
						parser->gasmix[mix_idx].helium = he;
						parser->gasmix[mix_idx].id = id;
						parser->ngasmixes = mix_idx + 1;
					}

					sample.gasmix = mix_idx;
					if (callback) callback(DC_SAMPLE_GASMIX, &sample, userdata);
					break;
				case EVENT_CHANGE_SETPOINT:
					// Ignore the 4 byte variant because it's not
					// supposed to be present in the sample data.
					if (event_info[i].size == 2) {
						sample.setpoint = data[offset + event_offset] / 10.0;
						if (callback) callback(DC_SAMPLE_SETPOINT, &sample, userdata);
					}
					break;
				case EVENT_SAMPLES_MISSED:
					count     = array_uint16_le(data + offset + event_offset);
					timestamp = array_uint32_le(data + offset + event_offset + 2);
					if (timestamp < time) {
						ERROR (abstract->context, "Timestamp moved backwards (%u %u).", timestamp, time);
						return DC_STATUS_DATAFORMAT;
					}
					nsamples += count;
					time = timestamp;
					break;
				default:
					WARNING (abstract->context, "Unknown event (%u %u).",
						event_info[i].type, event_info[i].size);
					break;
				}
				event_offset += event_info[i].size;
			}

			// Skip remaining sample bytes (if any).
			if (event_offset < length) {
				WARNING (abstract->context, "Remaining %u bytes skipped.", length - event_offset);
			}
			offset += length;
		}

		for (unsigned int i = 0; i < nconfig; ++i) {
			if (sample_info[i].divisor && (nsamples % sample_info[i].divisor) == 0) {
				if (offset + sample_info[i].size > size) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int value = 0;
				unsigned int deco_flags = 0, deco_ndl_tts = 0;
				unsigned int deco_depth = 0, deco_time = 0;
				switch (sample_info[i].type) {
				case SAMPLE_TEMPERATURE:
					value = array_uint16_le(data + offset);
					sample.temperature = value / 10.0;
					if (callback) callback(DC_SAMPLE_TEMPERATURE, &sample, userdata);
					break;
				case SAMPLE_CNS:
					value = array_uint16_le(data + offset);
					sample.cns = value / 10000.0;
					if (callback) callback (DC_SAMPLE_CNS, &sample, userdata);
					break;
				case SAMPLE_DECO_NDL:
					deco_flags   = data[offset];
					deco_ndl_tts = array_uint16_le(data + offset + 1);
					deco_depth   = array_uint16_le(data + offset + 3);
					deco_time    = array_uint16_le(data + offset + 5);
					if (deco_flags & DECOSTOP) {
						sample.deco.type = DC_DECO_DECOSTOP;
						sample.deco.depth = pressure_to_depth(deco_depth, atmospheric, density);
						sample.deco.time = deco_time;
					} else if (deco_flags & SAFETYSTOP) {
						sample.deco.type = DC_DECO_SAFETYSTOP;
						sample.deco.depth = pressure_to_depth(deco_depth, atmospheric, density);
						sample.deco.time = deco_time;
					} else {
						sample.deco.type = DC_DECO_NDL;
						sample.deco.depth = 0;
						sample.deco.time = deco_ndl_tts;
					}
					if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);
					break;
				default:
					break;
				}
				offset += sample_info[i].size;
			}
		}
	}

	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepsix_excursion_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < HEADERSIZE_MIN)
		return DC_STATUS_DATAFORMAT;

	unsigned int version = data[3];

	if (version == 0) {
		return deepsix_excursion_parser_samples_foreach_v0(abstract, callback, userdata);
	} else {
		return deepsix_excursion_parser_samples_foreach_v1(abstract, callback, userdata);
	}
}
