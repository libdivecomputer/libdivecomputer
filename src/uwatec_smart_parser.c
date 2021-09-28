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

#include <libdivecomputer/units.h>

#include "uwatec_smart.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &uwatec_smart_parser_vtable)

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

#define NBITS 8

#define SMARTPRO          0x10
#define GALILEO           0x11
#define ALADINTEC         0x12
#define ALADINTEC2G       0x13
#define SMARTCOM          0x14
#define ALADIN2G          0x15
#define ALADINSPORTMATRIX 0x17
#define SMARTTEC          0x18
#define GALILEOTRIMIX     0x19
#define SMARTZ            0x1C
#define MERIDIAN          0x20
#define ALADINSQUARE      0x22
#define CHROMIS           0x24
#define ALADINA1          0x25
#define MANTIS2           0x26
#define ALADINA2          0x28
#define G2                0x32
#define G2HUD             0x42

#define UNSUPPORTED 0xFFFFFFFF

#define NEVENTS   3
#define NGASMIXES 10

#define HEADER  1
#define PROFILE 2

#define FRESH 1000.0
#define SALT  1025.0

#define FREEDIVE  0x00000080
#define GAUGE     0x00001000
#define SALINITY  0x00100000

#define EPOCH 946684800 // 2000-01-01 00:00:00 UTC

typedef enum {
	PRESSURE_DEPTH,
	RBT,
	TEMPERATURE,
	PRESSURE,
	DEPTH,
	HEARTRATE,
	BEARING,
	ALARMS,
	TIME,
	APNEA,
	MISC,
} uwatec_smart_sample_t;

typedef enum {
	EV_WARNING,          /* Warning (yellow buzzer) */
	EV_ALARM,            /* Alarm (red buzzer) */
	EV_WORKLOAD,         /* Workload */
	EV_WORKLOAD_WARNING, /* Increased workload (lung symbol) */
	EV_BOOKMARK,         /* Bookmark / safety stop timer started */
	EV_GASMIX,           /* Active gasmix */
	EV_UNKNOWN,
} uwatec_smart_event_t;

typedef struct uwatec_smart_header_info_t {
	unsigned int maxdepth;
	unsigned int divetime;
	unsigned int gasmix;
	unsigned int ngases;
	unsigned int temp_minimum;
	unsigned int temp_maximum;
	unsigned int temp_surface;
	unsigned int tankpressure;
	unsigned int timezone;
	unsigned int settings;
} uwatec_smart_header_info_t;

typedef struct uwatec_smart_sample_info_t {
	uwatec_smart_sample_t type;
	unsigned int absolute;
	unsigned int index;
	unsigned int ntypebits;
	unsigned int ignoretype;
	unsigned int extrabytes;
} uwatec_smart_sample_info_t;

typedef struct uwatec_smart_event_info_t {
	uwatec_smart_event_t type;
	unsigned int mask;
	unsigned int shift;
} uwatec_smart_event_info_t;

typedef struct uwatec_smart_gasmix_t {
	unsigned int id;
	unsigned int oxygen;
	unsigned int helium;
} uwatec_smart_gasmix_t;

typedef struct uwatec_smart_tank_t {
	unsigned int id;
	unsigned int beginpressure;
	unsigned int endpressure;
	unsigned int gasmix;
} uwatec_smart_tank_t;

typedef struct uwatec_smart_parser_t uwatec_smart_parser_t;

struct uwatec_smart_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int devtime;
	dc_ticks_t systime;
	const uwatec_smart_sample_info_t *samples;
	const uwatec_smart_header_info_t *header;
	unsigned int headersize;
	unsigned int nsamples;
	const uwatec_smart_event_info_t *events[NEVENTS];
	unsigned int nevents[NEVENTS];
	unsigned int trimix;
	// Cached fields.
	unsigned int cached;
	unsigned int ngasmixes;
	uwatec_smart_gasmix_t gasmix[NGASMIXES];
	unsigned int ntanks;
	uwatec_smart_tank_t tank[NGASMIXES];
	dc_water_t watertype;
	dc_divemode_t divemode;
};

static dc_status_t uwatec_smart_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t uwatec_smart_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t uwatec_smart_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t uwatec_smart_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static dc_status_t uwatec_smart_parse (uwatec_smart_parser_t *parser, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t uwatec_smart_parser_vtable = {
	sizeof(uwatec_smart_parser_t),
	DC_FAMILY_UWATEC_SMART,
	uwatec_smart_parser_set_data, /* set_data */
	uwatec_smart_parser_get_datetime, /* datetime */
	uwatec_smart_parser_get_field, /* fields */
	uwatec_smart_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const
uwatec_smart_header_info_t uwatec_smart_pro_header = {
	18,
	20,
	24, 1,
	22, /* temp_minimum */
	UNSUPPORTED, /* temp_maximum */
	UNSUPPORTED, /* temp_surface */
	UNSUPPORTED, /* tankpressure */
	UNSUPPORTED, /* timezone */
	UNSUPPORTED, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_galileo_header = {
	22,
	26,
	44, 3,
	30, /* temp_minimum */
	28, /* temp_maximum */
	32, /* temp_surface */
	50, /* tankpressure */
	16, /* timezone */
	92, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_trimix_header = {
	22, /* maxdepth */
	26, /* divetime */
	UNSUPPORTED, 0, /* gasmixes */
	30, /* temp_minimum */
	28, /* temp_maximum */
	32, /* temp_surface */
	UNSUPPORTED, /* tankpressure */
	16, /* timezone */
	68, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_aladin_tec_header = {
	22,
	24,
	30, 1,
	26, /* temp_minimum */
	28, /* temp_maximum */
	32, /* temp_surface */
	UNSUPPORTED, /* tankpressure */
	16, /* timezone */
	52, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_aladin_tec2g_header = {
	22,
	26,
	34, 3,
	30, /* temp_minimum */
	28, /* temp_maximum */
	32, /* temp_surface */
	UNSUPPORTED, /* tankpressure */
	16, /* timezone */
	60, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_com_header = {
	18,
	20,
	24, 1,
	22, /* temp_minimum */
	UNSUPPORTED, /* temp_maximum */
	UNSUPPORTED, /* temp_surface */
	30, /* tankpressure */
	UNSUPPORTED, /* timezone */
	UNSUPPORTED, /* settings */
};

static const
uwatec_smart_header_info_t uwatec_smart_tec_header = {
	18,
	20,
	28, 3,
	22, /* temp_minimum */
	UNSUPPORTED, /* temp_maximum */
	UNSUPPORTED, /* temp_surface */
	34, /* tankpressure */
	UNSUPPORTED, /* timezone */
	UNSUPPORTED, /* settings */
};

static const
uwatec_smart_sample_info_t uwatec_smart_pro_samples[] = {
	{DEPTH,          0, 0, 1, 0, 0}, // 0ddddddd
	{TEMPERATURE,    0, 0, 2, 0, 0}, // 10dddddd
	{TIME,           1, 0, 3, 0, 0}, // 110ddddd
	{ALARMS,         1, 0, 4, 0, 0}, // 1110dddd
	{DEPTH,          0, 0, 5, 0, 1}, // 11110ddd dddddddd
	{TEMPERATURE,    0, 0, 6, 0, 1}, // 111110dd dddddddd
	{DEPTH,          1, 0, 7, 1, 2}, // 1111110d dddddddd dddddddd
	{TEMPERATURE,    1, 0, 8, 0, 2}, // 11111110 dddddddd dddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_galileo_samples[] = {
	{DEPTH,          0, 0, 1, 0, 0}, // 0ddd dddd
	{RBT,            0, 0, 3, 0, 0}, // 100d dddd
	{PRESSURE,       0, 0, 4, 0, 0}, // 1010 dddd
	{TEMPERATURE,    0, 0, 4, 0, 0}, // 1011 dddd
	{TIME,           1, 0, 4, 0, 0}, // 1100 dddd
	{HEARTRATE,      0, 0, 4, 0, 0}, // 1101 dddd
	{ALARMS,         1, 0, 4, 0, 0}, // 1110 dddd
	{ALARMS,         1, 1, 8, 0, 1}, // 1111 0000 dddddddd
	{DEPTH,          1, 0, 8, 0, 2}, // 1111 0001 dddddddd dddddddd
	{RBT,            1, 0, 8, 0, 1}, // 1111 0010 dddddddd
	{TEMPERATURE,    1, 0, 8, 0, 2}, // 1111 0011 dddddddd dddddddd
	{PRESSURE,       1, 0, 8, 0, 2}, // 1111 0100 dddddddd dddddddd
	{PRESSURE,       1, 1, 8, 0, 2}, // 1111 0101 dddddddd dddddddd
	{PRESSURE,       1, 2, 8, 0, 2}, // 1111 0110 dddddddd dddddddd
	{HEARTRATE,      1, 0, 8, 0, 1}, // 1111 0111 dddddddd
	{BEARING,        1, 0, 8, 0, 2}, // 1111 1000 dddddddd dddddddd
	{ALARMS,         1, 2, 8, 0, 1}, // 1111 1001 dddddddd
	{APNEA,          1, 0, 8, 0, 0}, // 1111 1010 (8 bytes)
	{MISC,           1, 0, 8, 0, 1}, // 1111 1011 dddddddd (n-1 bytes)
};


static const
uwatec_smart_sample_info_t uwatec_smart_aladin_samples[] = {
	{DEPTH,          0, 0, 1, 0, 0}, // 0ddddddd
	{TEMPERATURE,    0, 0, 2, 0, 0}, // 10dddddd
	{TIME,           1, 0, 3, 0, 0}, // 110ddddd
	{ALARMS,         1, 0, 4, 0, 0}, // 1110dddd
	{DEPTH,          0, 0, 5, 0, 1}, // 11110ddd dddddddd
	{TEMPERATURE,    0, 0, 6, 0, 1}, // 111110dd dddddddd
	{DEPTH,          1, 0, 7, 1, 2}, // 1111110d dddddddd dddddddd
	{TEMPERATURE,    1, 0, 8, 0, 2}, // 11111110 dddddddd dddddddd
	{ALARMS,         1, 1, 9, 0, 0}, // 11111111 0ddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_com_samples[] = {
	{PRESSURE_DEPTH, 0, 0,  1, 0, 1}, // 0ddddddd dddddddd
	{RBT,            0, 0,  2, 0, 0}, // 10dddddd
	{TEMPERATURE,    0, 0,  3, 0, 0}, // 110ddddd
	{PRESSURE,       0, 0,  4, 0, 1}, // 1110dddd dddddddd
	{DEPTH,          0, 0,  5, 0, 1}, // 11110ddd dddddddd
	{TEMPERATURE,    0, 0,  6, 0, 1}, // 111110dd dddddddd
	{ALARMS,         1, 0,  7, 1, 1}, // 1111110d dddddddd
	{TIME,           1, 0,  8, 0, 1}, // 11111110 dddddddd
	{DEPTH,          1, 0,  9, 1, 2}, // 11111111 0ddddddd dddddddd dddddddd
	{PRESSURE,       1, 0, 10, 1, 2}, // 11111111 10dddddd dddddddd dddddddd
	{TEMPERATURE,    1, 0, 11, 1, 2}, // 11111111 110ddddd dddddddd dddddddd
	{RBT,            1, 0, 12, 1, 1}, // 11111111 1110dddd dddddddd
};

static const
uwatec_smart_sample_info_t uwatec_smart_tec_samples[] = {
	{PRESSURE_DEPTH, 0, 0,  1, 0, 1}, // 0ddddddd dddddddd
	{RBT,            0, 0,  2, 0, 0}, // 10dddddd
	{TEMPERATURE,    0, 0,  3, 0, 0}, // 110ddddd
	{PRESSURE,       0, 0,  4, 0, 1}, // 1110dddd dddddddd
	{DEPTH,          0, 0,  5, 0, 1}, // 11110ddd dddddddd
	{TEMPERATURE,    0, 0,  6, 0, 1}, // 111110dd dddddddd
	{ALARMS,         1, 0,  7, 1, 1}, // 1111110d dddddddd
	{TIME,           1, 0,  8, 0, 1}, // 11111110 dddddddd
	{DEPTH,          1, 0,  9, 1, 2}, // 11111111 0ddddddd dddddddd dddddddd
	{TEMPERATURE,    1, 0, 10, 1, 2}, // 11111111 10dddddd dddddddd dddddddd
	{PRESSURE,       1, 0, 11, 1, 2}, // 11111111 110ddddd dddddddd dddddddd
	{PRESSURE,       1, 1, 12, 1, 2}, // 11111111 1110dddd dddddddd dddddddd
	{PRESSURE,       1, 2, 13, 1, 2}, // 11111111 11110ddd dddddddd dddddddd
	{RBT,            1, 0, 14, 1, 1}, // 11111111 111110dd dddddddd
};

static const
uwatec_smart_event_info_t uwatec_smart_tec_events_0[] = {
	{EV_WARNING,          0x01, 0},
	{EV_ALARM,            0x02, 1},
	{EV_WORKLOAD_WARNING, 0x04, 2},
	{EV_WORKLOAD,         0x38, 3},
	{EV_UNKNOWN,          0xC0, 6},
};

static const
uwatec_smart_event_info_t uwatec_smart_aladintec_events_0[] = {
	{EV_WARNING,          0x01, 0},
	{EV_ALARM,            0x02, 1},
	{EV_BOOKMARK,         0x04, 2},
	{EV_UNKNOWN,          0x08, 3},
};

static const
uwatec_smart_event_info_t uwatec_smart_aladintec_events_1[] = {
	{EV_UNKNOWN,          0xFF, 0},
};

static const
uwatec_smart_event_info_t uwatec_smart_aladintec2g_events_0[] = {
	{EV_WARNING,          0x01, 0},
	{EV_ALARM,            0x02, 1},
	{EV_BOOKMARK,         0x04, 2},
	{EV_UNKNOWN,          0x08, 3},
};

static const
uwatec_smart_event_info_t uwatec_smart_aladintec2g_events_1[] = {
	{EV_UNKNOWN,          0x07, 0},
	{EV_GASMIX,           0x18, 3},
};

static const
uwatec_smart_event_info_t uwatec_smart_galileo_events_0[] = {
	{EV_WARNING,          0x01, 0},
	{EV_ALARM,            0x02, 1},
	{EV_WORKLOAD_WARNING, 0x04, 2},
	{EV_BOOKMARK,         0x08, 3},
};

static const
uwatec_smart_event_info_t uwatec_smart_galileo_events_1[] = {
	{EV_WORKLOAD,         0x07, 0},
	{EV_UNKNOWN,          0x18, 3},
	{EV_GASMIX,           0x60, 5},
	{EV_UNKNOWN,          0x80, 7},
};

static const
uwatec_smart_event_info_t uwatec_smart_galileo_events_2[] = {
	{EV_UNKNOWN,          0xFF, 0},
};

static const
uwatec_smart_event_info_t uwatec_smart_trimix_events_2[] = {
	{EV_UNKNOWN,          0x0F, 0},
	{EV_GASMIX,           0xF0, 4},
};

static unsigned int
uwatec_smart_find_gasmix (uwatec_smart_parser_t *parser, unsigned int id)
{
       unsigned int i = 0;
       while (i < parser->ngasmixes) {
               if (id == parser->gasmix[i].id)
                       break;
               i++;
       }

       return i;
}

static unsigned int
uwatec_smart_find_tank (uwatec_smart_parser_t *parser, unsigned int id)
{
       unsigned int i = 0;
       while (i < parser->ntanks) {
               if (id == parser->tank[i].id)
                       break;
               i++;
       }

       return i;
}

static dc_status_t
uwatec_smart_parser_cache (uwatec_smart_parser_t *parser)
{
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	if (parser->model == GALILEO || parser->model == GALILEOTRIMIX) {
		if (size < 44)
			return DC_STATUS_DATAFORMAT;

		if (data[43] & 0x80) {
			parser->trimix = 1;
			parser->headersize = 84;
			parser->header = &uwatec_smart_trimix_header;
			parser->events[2] = uwatec_smart_trimix_events_2;
			parser->nevents[2] = C_ARRAY_SIZE (uwatec_smart_trimix_events_2);
		} else {
			parser->trimix = 0;
			parser->headersize = 152;
			parser->header = &uwatec_smart_galileo_header;
			parser->events[2] = uwatec_smart_galileo_events_2;
			parser->nevents[2] = C_ARRAY_SIZE (uwatec_smart_galileo_events_2);
		}
	}

	const uwatec_smart_header_info_t *header = parser->header;

	// Get the settings.
	dc_divemode_t divemode = DC_DIVEMODE_OC;
	dc_water_t watertype = DC_WATER_FRESH;
	if (header->settings != UNSUPPORTED) {
		unsigned int settings = array_uint32_le (data + header->settings);

		// Get the freedive/gauge bits.
		unsigned int freedive = 0;
		unsigned int gauge = (settings & GAUGE) != 0;
		if (parser->model != ALADINTEC && parser->model != ALADINTEC2G) {
			freedive = (settings & FREEDIVE) != 0;
		}

		// Get the dive mode. The freedive bit needs to be checked
		// first, because freedives have both the freedive and gauge
		// bits set.
		if (freedive) {
			divemode = DC_DIVEMODE_FREEDIVE;
		} else if (gauge) {
			divemode = DC_DIVEMODE_GAUGE;
		} else {
			divemode = DC_DIVEMODE_OC;
		}

		// Get the water type.
		if (settings & SALINITY) {
			watertype = DC_WATER_SALT;
		}
	}

	// Get the gas mixes and tanks.
	unsigned int ntanks = 0;
	unsigned int ngasmixes = 0;
	uwatec_smart_tank_t tank[NGASMIXES] = {{0}};
	uwatec_smart_gasmix_t gasmix[NGASMIXES] = {{0}};
	if (header->gasmix != UNSUPPORTED) {
		for (unsigned int i = 0; i < header->ngases; ++i) {
			unsigned int idx = DC_GASMIX_UNKNOWN;
			unsigned int o2 = 0;
			if (parser->model == ALADINTEC2G) {
				o2 = data[header->gasmix + i];
			} else {
				o2 = array_uint16_le (data + header->gasmix + i * 2);
			}

			if (o2 != 0) {
				idx = ngasmixes;
				gasmix[ngasmixes].id = i;
				gasmix[ngasmixes].oxygen = o2;
				gasmix[ngasmixes].helium = 0;
				ngasmixes++;
			}

			unsigned int beginpressure = 0;
			unsigned int endpressure = 0;
			if (header->tankpressure != UNSUPPORTED &&
				divemode != DC_DIVEMODE_FREEDIVE) {
				if (parser->model == GALILEO || parser->model == GALILEOTRIMIX ||
					parser->model == ALADIN2G || parser->model == MERIDIAN ||
					parser->model == CHROMIS || parser->model == MANTIS2 ||
					parser->model == G2 || parser->model == ALADINSPORTMATRIX ||
					parser->model == ALADINSQUARE || parser->model == G2HUD ||
					parser->model == ALADINA1 || parser->model == ALADINA2 ) {
					unsigned int offset = header->tankpressure + 2 * i;
					endpressure   = array_uint16_le(data + offset);
					beginpressure = array_uint16_le(data + offset + 2 * header->ngases);
				} else {
					unsigned int offset = header->tankpressure + 4 * i;
					beginpressure = array_uint16_le(data + offset);
					endpressure   = array_uint16_le(data + offset + 2);
				}
			}
			if ((beginpressure != 0 || endpressure != 0) &&
				(beginpressure != 0xFFFF) && (endpressure != 0xFFFF)) {
				tank[ntanks].id = i;
				tank[ntanks].beginpressure = beginpressure;
				tank[ntanks].endpressure = endpressure;
				tank[ntanks].gasmix = idx;
				ntanks++;
			}
		}
	}

	// Cache the data for later use.
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->ntanks = ntanks;
	for (unsigned int i = 0; i < ntanks; ++i) {
		parser->tank[i] = tank[i];
	}
	parser->watertype = watertype;
	parser->divemode = divemode;
	parser->cached = HEADER;

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_smart_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int devtime, dc_ticks_t systime)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_smart_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (uwatec_smart_parser_t *) dc_parser_allocate (context, &uwatec_smart_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->devtime = devtime;
	parser->systime = systime;
	parser->trimix = 0;
	for (unsigned int i = 0; i < NEVENTS; ++i) {
		parser->events[i] = NULL;
		parser->nevents[i] = 0;
	}
	switch (model) {
	case SMARTPRO:
		parser->headersize = 92;
		parser->header = &uwatec_smart_pro_header;
		parser->samples = uwatec_smart_pro_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_pro_samples);
		parser->events[0] = uwatec_smart_tec_events_0;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_tec_events_0);
		break;
	case GALILEO:
	case GALILEOTRIMIX:
	case ALADIN2G:
	case MERIDIAN:
	case CHROMIS:
	case MANTIS2:
	case ALADINSQUARE:
		parser->headersize = 152;
		parser->header = &uwatec_smart_galileo_header;
		parser->samples = uwatec_smart_galileo_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_galileo_samples);
		parser->events[0] = uwatec_smart_galileo_events_0;
		parser->events[1] = uwatec_smart_galileo_events_1;
		parser->events[2] = uwatec_smart_galileo_events_2;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_galileo_events_0);
		parser->nevents[1] = C_ARRAY_SIZE (uwatec_smart_galileo_events_1);
		parser->nevents[2] = C_ARRAY_SIZE (uwatec_smart_galileo_events_2);
		break;
	case G2:
	case G2HUD:
	case ALADINSPORTMATRIX:
	case ALADINA1:
	case ALADINA2:
		parser->headersize = 84;
		parser->header = &uwatec_smart_trimix_header;
		parser->samples = uwatec_smart_galileo_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_galileo_samples);
		parser->events[0] = uwatec_smart_galileo_events_0;
		parser->events[1] = uwatec_smart_galileo_events_1;
		parser->events[2] = uwatec_smart_trimix_events_2;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_galileo_events_0);
		parser->nevents[1] = C_ARRAY_SIZE (uwatec_smart_galileo_events_1);
		parser->nevents[2] = C_ARRAY_SIZE (uwatec_smart_trimix_events_2);
		parser->trimix = 1;
		break;
	case ALADINTEC:
		parser->headersize = 108;
		parser->header = &uwatec_smart_aladin_tec_header;
		parser->samples = uwatec_smart_aladin_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_aladin_samples);
		parser->events[0] = uwatec_smart_aladintec_events_0;
		parser->events[1] = uwatec_smart_aladintec_events_1;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_aladintec_events_0);
		parser->nevents[1] = C_ARRAY_SIZE (uwatec_smart_aladintec_events_1);
		break;
	case ALADINTEC2G:
		parser->headersize = 116;
		parser->header = &uwatec_smart_aladin_tec2g_header;
		parser->samples = uwatec_smart_aladin_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_aladin_samples);
		parser->events[0] = uwatec_smart_aladintec2g_events_0;
		parser->events[1] = uwatec_smart_aladintec2g_events_1;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_aladintec2g_events_0);
		parser->nevents[1] = C_ARRAY_SIZE (uwatec_smart_aladintec2g_events_1);
		break;
	case SMARTCOM:
		parser->headersize = 100;
		parser->header = &uwatec_smart_com_header;
		parser->samples = uwatec_smart_com_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_com_samples);
		parser->events[0] = uwatec_smart_tec_events_0;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_tec_events_0);
		break;
	case SMARTTEC:
	case SMARTZ:
		parser->headersize = 132;
		parser->header = &uwatec_smart_tec_header;
		parser->samples = uwatec_smart_tec_samples;
		parser->nsamples = C_ARRAY_SIZE (uwatec_smart_tec_samples);
		parser->events[0] = uwatec_smart_tec_events_0;
		parser->nevents[0] = C_ARRAY_SIZE (uwatec_smart_tec_events_0);
		break;
	default:
		status = DC_STATUS_INVALIDARGS;
		goto error_free;
	}

	parser->cached = 0;
	parser->ngasmixes = 0;
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].id = 0;
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
		parser->tank[i].id = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tank[i].gasmix = 0;
	}
	parser->watertype = DC_WATER_FRESH;
	parser->divemode = DC_DIVEMODE_OC;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;

error_free:
	dc_parser_deallocate ((dc_parser_t *) parser);
	return status;
}


static dc_status_t
uwatec_smart_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	uwatec_smart_parser_t *parser = (uwatec_smart_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->ngasmixes = 0;
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].id = 0;
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
		parser->tank[i].id = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tank[i].gasmix = 0;
	}
	parser->watertype = DC_WATER_FRESH;
	parser->divemode = DC_DIVEMODE_OC;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	uwatec_smart_parser_t *parser = (uwatec_smart_parser_t *) abstract;
	const uwatec_smart_header_info_t *table = parser->header;
	const unsigned char *data = abstract->data;

	if (abstract->size < parser->headersize)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (abstract->data + 8);

	dc_ticks_t ticks = EPOCH + timestamp / 2;

	if (table->timezone != UNSUPPORTED) {
		// For devices with timezone support, the UTC offset of the
		// device is used. The UTC offset is stored in units of 15
		// minutes (or 900 seconds).
		int utc_offset = (signed char) data[table->timezone];

		ticks += utc_offset * 900;

		if (!dc_datetime_gmtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;

		datetime->timezone = utc_offset * 900;
	} else {
		// For devices without timezone support, the current timezone of
		// the host system is used.
		if (!dc_datetime_localtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	uwatec_smart_parser_t *parser = (uwatec_smart_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = uwatec_smart_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Cache the profile data.
	if (parser->cached < PROFILE) {
		rc = uwatec_smart_parse (parser, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	const uwatec_smart_header_info_t *table = parser->header;
	const unsigned char *data = abstract->data;

	double density = (parser->watertype == DC_WATER_SALT ? SALT : FRESH);

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + table->divetime) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + table->maxdepth) * (BAR / 1000.0) / (density * 10.0);
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->beginpressure = parser->tank[flags].beginpressure / 128.0;
			tank->endpressure   = parser->tank[flags].endpressure   / 128.0;
			tank->gasmix = parser->tank[flags].gasmix;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed short) array_uint16_le (data + table->temp_minimum) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			if (table->temp_maximum == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = (signed short) array_uint16_le (data + table->temp_maximum) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_SURFACE:
			if (table->temp_surface == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = (signed short) array_uint16_le (data + table->temp_surface) / 10.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (table->settings == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			*((dc_divemode_t *) value) = parser->divemode;
			break;
		case DC_FIELD_SALINITY:
			if (table->settings == UNSUPPORTED)
				return DC_STATUS_UNSUPPORTED;
			water->type = parser->watertype;
			water->density = density;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
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

	return (unsigned int) -1;
}


static unsigned int
uwatec_galileo_identify (unsigned char value)
{
	// Bits: 0ddd dddd
	if ((value & 0x80) == 0)
		return 0;

	// Bits: 100d dddd
	if ((value & 0xE0) == 0x80)
		return 1;

	// Bits: 1XXX dddd
	if ((value & 0xF0) != 0xF0)
		return (value & 0x70) >> 4;

	// Bits: 1111 XXXX
	return (value & 0x0F) + 7;
}


static unsigned int
uwatec_smart_fixsignbit (unsigned int x, unsigned int n)
{
	if (n <= 0 || n > 32)
		return 0;

	unsigned int signbit = (1 << (n - 1));
	unsigned int mask = (signbit - 1);

	// When turning a two's-complement number with a certain number
	// of bits into one with more bits, the sign bit must be repeated
	// in all the extra bits.
	if ((x & signbit) == signbit)
		return x | ~mask;
	else
		return x & mask;
}


static dc_status_t
uwatec_smart_parse (uwatec_smart_parser_t *parser, dc_sample_callback_t callback, void *userdata)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	const uwatec_smart_sample_info_t *table = parser->samples;
	unsigned int entries = parser->nsamples;

	// Get the maximum number of alarm bytes.
	unsigned int nalarms = 0;
	for (unsigned int i = 0; i < entries; ++i) {
		if (table[i].type == ALARMS &&
			table[i].index >= nalarms)
		{
			nalarms = table[i].index + 1;
		}
	}

	int complete = 0;
	int calibrated = 0;

	unsigned int time = 0;
	unsigned int rbt = 99;
	unsigned int tank = 0;
	unsigned int gasmix = 0;
	unsigned int depth = 0, depth_calibration = 0;
	int temperature = 0;
	unsigned int pressure = 0;
	unsigned int heartrate = 0;
	unsigned int bearing = 0;
	unsigned int bookmark = 0;

	// Previous gas mix - initialize with impossible value
	unsigned int gasmix_previous = 0xFFFFFFFF;

	double density = (parser->watertype == DC_WATER_SALT ? SALT : FRESH);

	unsigned int interval = 4;
	if (parser->divemode == DC_DIVEMODE_FREEDIVE) {
		interval = 1;
	}

	int have_depth = 0, have_temperature = 0, have_pressure = 0, have_rbt = 0,
		have_heartrate = 0, have_bearing = 0;

	unsigned int offset = parser->headersize;
	while (offset < size) {
		dc_sample_value_t sample = {0};

		// Process the type bits in the bitstream.
		unsigned int id = 0;
		if (parser->model == GALILEO || parser->model == GALILEOTRIMIX ||
			parser->model == ALADIN2G || parser->model == MERIDIAN ||
			parser->model == CHROMIS || parser->model == MANTIS2 ||
			parser->model == G2 || parser->model == ALADINSPORTMATRIX ||
			parser->model == ALADINSQUARE || parser->model == G2HUD ||
			parser->model == ALADINA1 || parser->model == ALADINA2 ) {
			// Uwatec Galileo
			id = uwatec_galileo_identify (data[offset]);
		} else {
			// Uwatec Smart
			id = uwatec_smart_identify (data + offset, size - offset);
		}
		if (id >= entries) {
			ERROR (abstract->context, "Invalid type bits.");
			return DC_STATUS_DATAFORMAT;
		}

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

		// Check for buffer overflows.
		if (offset + table[id].extrabytes > size) {
			ERROR (abstract->context, "Incomplete sample data.");
			return DC_STATUS_DATAFORMAT;
		}

		// Process the extra data bytes.
		for (unsigned int i = 0; i < table[id].extrabytes; ++i) {
			nbits += NBITS;
			value <<= NBITS;
			value += data[offset];
			offset++;
		}

		// Fix the sign bit.
		signed int svalue = uwatec_smart_fixsignbit (value, nbits);

		// Parse the value.
		unsigned int idx = 0;
		unsigned int subtype = 0;
		unsigned int nevents = 0;
		const uwatec_smart_event_info_t *events = NULL;
		switch (table[id].type) {
		case PRESSURE_DEPTH:
			pressure += (signed char) ((svalue >> NBITS) & 0xFF);
			depth += (signed char) (svalue & 0xFF);
			complete = 1;
			break;
		case RBT:
			if (table[id].absolute) {
				rbt = value;
				have_rbt = 1;
			} else {
				rbt += svalue;
			}
			break;
		case TEMPERATURE:
			if (table[id].absolute) {
				temperature = svalue;
				have_temperature = 1;
			} else {
				temperature += svalue;
			}
			break;
		case PRESSURE:
			if (table[id].absolute) {
				if (parser->trimix) {
					tank = (value & 0xF000) >> 12;
					pressure = (value & 0x0FFF);
				} else {
					tank = table[id].index;
					pressure = value;
				}
				have_pressure = 1;
				gasmix = tank;
			} else {
				pressure += svalue;
			}
			break;
		case DEPTH:
			if (table[id].absolute) {
				depth = value;
				if (!calibrated) {
					calibrated = 1;
					depth_calibration = depth;
				}
				have_depth = 1;
			} else {
				depth += svalue;
			}
			complete = 1;
			break;
		case HEARTRATE:
			if (table[id].absolute) {
				heartrate = value;
				have_heartrate = 1;
			} else {
				heartrate += svalue;
			}
			break;
		case BEARING:
			bearing = value;
			have_bearing = 1;
			break;
		case ALARMS:
			idx = table[id].index;
			if (idx >= NEVENTS || parser->events[idx] == NULL) {
				ERROR (abstract->context, "Unexpected event index.");
				return DC_STATUS_DATAFORMAT;
			}

			events = parser->events[idx];
			nevents = parser->nevents[idx];

			for (unsigned int i = 0; i < nevents; ++i) {
				uwatec_smart_event_t ev_type = events[i].type;
				unsigned int ev_value = (value & events[i].mask) >> events[i].shift;
				switch (ev_type) {
				case EV_BOOKMARK:
					bookmark = ev_value;
					break;
				case EV_GASMIX:
					gasmix = ev_value;
					break;
				default:
					break;
				}
			}
			break;
		case TIME:
			complete = value;
			break;
		case APNEA:
			if (offset + 8 > size) {
				ERROR (abstract->context, "Incomplete sample data.");
				return DC_STATUS_DATAFORMAT;
			}
			offset += 8;
			break;
		case MISC:
			if (value < 1 || offset + value - 1 > size) {
				ERROR (abstract->context, "Incomplete sample data.");
				return DC_STATUS_DATAFORMAT;
			}

			subtype = data[offset];
			if (subtype >= 32 && subtype <= 41) {
				if (value < 16) {
					ERROR (abstract->context, "Incomplete sample data.");
					return DC_STATUS_DATAFORMAT;
				}
				unsigned int mixid = subtype - 32;
				unsigned int mixidx = DC_GASMIX_UNKNOWN;
				unsigned int o2 = array_uint16_le (data + offset + 1);
				unsigned int he = array_uint16_le (data + offset + 3);
				unsigned int beginpressure = array_uint16_le (data + offset + 5);
				unsigned int endpressure   = array_uint16_le (data + offset + 7);

				if (o2 != 0 || he != 0) {
					idx = uwatec_smart_find_gasmix (parser, mixid);
					if (idx >= parser->ngasmixes) {
						if (idx >= NGASMIXES) {
							ERROR (abstract->context, "Maximum number of gas mixes reached.");
							return DC_STATUS_NOMEMORY;
						}
						parser->gasmix[idx].id = mixid;
						parser->gasmix[idx].oxygen = o2;
						parser->gasmix[idx].helium = he;
						parser->ngasmixes++;
					}
					mixidx = idx;
				}

				if ((beginpressure != 0 || endpressure != 0) &&
					(beginpressure != 0xFFFF) && (endpressure != 0xFFFF)) {
					idx = uwatec_smart_find_tank (parser, mixid);
					if (idx >= parser->ntanks) {
						if (idx >= NGASMIXES) {
							ERROR (abstract->context, "Maximum number of tanks reached.");
							return DC_STATUS_NOMEMORY;
						}
						parser->tank[idx].id = mixid;
						parser->tank[idx].beginpressure = beginpressure;
						parser->tank[idx].endpressure = endpressure;
						parser->tank[idx].gasmix = mixidx;
						parser->ntanks++;
					}
				}
			}

			offset += value - 1;
			break;
		default:
			WARNING (abstract->context, "Unknown sample type.");
			break;
		}

		while (complete) {
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			if (parser->ngasmixes && gasmix != gasmix_previous) {
				idx = uwatec_smart_find_gasmix (parser, gasmix);
				if (idx >= parser->ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix index.");
					return DC_STATUS_DATAFORMAT;
				}
				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				gasmix_previous = gasmix;
			}

			if (have_temperature) {
				sample.temperature = temperature / 2.5;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
			}

			if (bookmark) {
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}

			if (have_rbt || have_pressure) {
				sample.rbt = rbt;
				if (callback) callback (DC_SAMPLE_RBT, sample, userdata);
			}

			if (have_pressure) {
				idx = uwatec_smart_find_tank(parser, tank);
				if (idx < parser->ntanks) {
					sample.pressure.tank = idx;
					sample.pressure.value = pressure / 4.0;
					if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
				}
			}

			if (have_heartrate) {
				sample.heartbeat = heartrate;
				if (callback) callback (DC_SAMPLE_HEARTBEAT, sample, userdata);
			}

			if (have_bearing) {
				sample.bearing = bearing;
				if (callback) callback (DC_SAMPLE_BEARING, sample, userdata);
				have_bearing = 0;
			}

			if (have_depth) {
				sample.depth = (signed int)(depth - depth_calibration) * (2.0 * BAR / 1000.0) / (density * 10.0);
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
			}

			time += interval;
			complete--;
		}
	}

	parser->cached = PROFILE;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	uwatec_smart_parser_t *parser = (uwatec_smart_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = uwatec_smart_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Cache the profile data.
	if (parser->cached < PROFILE) {
		rc = uwatec_smart_parse (parser, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	return uwatec_smart_parse (parser, callback, userdata);
}
