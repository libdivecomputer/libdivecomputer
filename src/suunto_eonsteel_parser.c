/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Linus Torvalds
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

#include <libdivecomputer/suunto_eonsteel.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

struct type_desc {
	const char *desc, *format, *mod;
};

#define MAXTYPE 512
#define MAXGASES 16

typedef struct suunto_eonsteel_parser_t {
	dc_parser_t base;
	struct type_desc type_desc[MAXTYPE];
	// field cache
	struct {
		unsigned int initialized;
		unsigned int divetime;
		double maxdepth;
		double avgdepth;
		unsigned int ngases;
		dc_gasmix_t gasmix[MAXGASES];
		dc_salinity_t salinity;
		double surface_pressure;
	} cache;
} suunto_eonsteel_parser_t;

typedef int (*eon_data_cb_t)(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user);

static void
desc_free (struct type_desc desc[], unsigned int count)
{
	for (unsigned int i = 0; i < count; ++i) {
		free((void *)desc[i].desc);
		free((void *)desc[i].format);
		free((void *)desc[i].mod);
	}
}

static int record_type(suunto_eonsteel_parser_t *eon, unsigned short type, const char *name, int namelen)
{
	struct type_desc desc;
	const char *next;

	desc.desc = desc.format = desc.mod = NULL;
	do {
		int len;
		char *p;

		next = strchr(name, '\n');
		if (next) {
			len = next - name;
			next++;
		} else {
			len = strlen(name);
		}

		if (len < 5 || name[0] != '<' || name[4] != '>') {
			ERROR(eon->base.context, "Unexpected type description: %.*s", len, name);
			return -1;
		}
		p = (char *) malloc(len-4);
		if (!p) {
			ERROR(eon->base.context, "out of memory");
			desc_free(&desc, 1);
			return -1;
		}
		memcpy(p, name+5, len-5);
		p[len-5] = 0;

		// PTH, GRP, FRM, MOD
		switch (name[1]) {
		case 'P':
		case 'G':
			desc.desc = p;
			break;
		case 'F':
			desc.format = p;
			break;
		case 'M':
			desc.mod = p;
			break;
		default:
			ERROR(eon->base.context, "Unknown type descriptor: %.*s", len, name);
			desc_free(&desc, 1);
			free(p);
			return -1;
		}
	} while ((name = next) != NULL);

	if (type > MAXTYPE) {
		ERROR(eon->base.context, "Type out of range (%04x: '%s' '%s' '%s')",
			type,
			desc.desc ? desc.desc : "",
			desc.format ? desc.format : "",
			desc.mod ? desc.mod : "");
		desc_free(&desc, 1);
		return -1;
	}

	desc_free(eon->type_desc + type, 1);
	eon->type_desc[type] = desc;
	return 0;
}

static int traverse_entry(suunto_eonsteel_parser_t *eon, const unsigned char *p, int len, eon_data_cb_t callback, void *user)
{
	const unsigned char *name, *data, *end, *last, *one_past_end = p + len;
	int textlen, type;
	int rc;

	// First two bytes: zero and text length
	if (p[0]) {
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "next", p, 8);
		ERROR(eon->base.context, "Bad dive entry (%02x)", p[0]);
		return -1;
	}
	textlen = p[1];

	name = p + 2;
	if (textlen == 0xff) {
		textlen = array_uint32_le(name);
		name += 4;
	}

	// Two bytes of 'type' followed by the name/descriptor, followed by the data
	data = name + textlen;
	type = array_uint16_le(name);
	name += 2;

	if (*name != '<') {
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "bad", p, 16);
		return -1;
	}

	record_type(eon, type, (const char *) name, textlen-3);

	end = data;
	last = data;
	while (end < one_past_end && *end) {
		const unsigned char *begin = end;
		unsigned int type = *end++;
		unsigned int len;
		if (type == 0xff) {
			type = array_uint16_le(end);
			end += 2;
		}
		len = *end++;

		// I've never actually seen this case yet..
		// Just assuming from the other cases.
		if (len == 0xff) {
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "len-ff", end, 8);
			len = array_uint32_le(end);
			end += 4;
		}

		if (type > MAXTYPE || !eon->type_desc[type].desc) {
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "last", last, 16);
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "this", begin, 16);
		} else {
			rc = callback(type, eon->type_desc+type, end, len, user);
			if (rc < 0)
				return rc;
		}

		last = begin;
		end += len;
	}

	return end - p;
}

static int traverse_data(suunto_eonsteel_parser_t *eon, eon_data_cb_t callback, void *user)
{
	const unsigned char *data = eon->base.data;
	int len = eon->base.size;

	// Dive files start with "SBEM" and four NUL characters
	// Additionally, we've prepended the time as an extra
	// 4-byte pre-header
	if (len < 12 || memcmp(data+4, "SBEM", 4))
		return 0;

	data += 12;
	len -= 12;

	while (len > 4) {
		int i = traverse_entry(eon, data, len, callback, user);
		if (i < 0)
			return 1;
		len -= i;
		data += i;
	}
	return 0;
}

struct sample_data {
	suunto_eonsteel_parser_t *eon;
	dc_sample_callback_t callback;
	void *userdata;
	unsigned int time;
};

static void sample_time(struct sample_data *info, unsigned short time_delta)
{
	dc_sample_value_t sample = {0};

	info->time += time_delta;
	sample.time = info->time / 1000;
	if (info->callback) info->callback(DC_SAMPLE_TIME, sample, info->userdata);
}

static void sample_depth(struct sample_data *info, unsigned short depth)
{
	dc_sample_value_t sample = {0};

	if (depth == 0xffff)
		return;

	sample.depth = depth / 100.0;
	if (info->callback) info->callback(DC_SAMPLE_DEPTH, sample, info->userdata);
}

static void sample_temp(struct sample_data *info, short temp)
{
	dc_sample_value_t sample = {0};

	if (temp < -3000)
		return;

	sample.temperature = temp / 10.0;
	if (info->callback) info->callback(DC_SAMPLE_TEMPERATURE, sample, info->userdata);
}

static void sample_deco(struct sample_data *info, short ndl, unsigned short tts, unsigned ceiling)
{
	dc_sample_value_t sample = {0};

	/* Are we in deco? */
	if (ndl < 0) {
		sample.deco.type = DC_DECO_DECOSTOP;
		if (tts != 0xffff)
			sample.deco.time = tts;
		if (ceiling != 0xffff)
			sample.deco.depth = ceiling / 100.0;
	} else {
		sample.deco.type = DC_DECO_NDL;
		sample.deco.time = ndl;
	}
	if (info->callback) info->callback(DC_SAMPLE_DECO, sample, info->userdata);
}

static void sample_cylinder_pressure(struct sample_data *info, unsigned char idx, unsigned short pressure)
{
	dc_sample_value_t sample = {0};

	if (pressure == 0xffff)
		return;

	sample.pressure.tank = idx-1;
	sample.pressure.value = pressure / 100.0;
	if (info->callback) info->callback(DC_SAMPLE_PRESSURE, sample, info->userdata);
}

static void sample_gas_switch_event(struct sample_data *info, unsigned short idx)
{
	suunto_eonsteel_parser_t *eon = info->eon;
	dc_sample_value_t sample = {0};
	int o2, he;

	if (idx < 1 || idx > eon->cache.ngases)
		return;

	// Horrible, broken, gas change events
	o2 = 100 * eon->cache.gasmix[idx-1].oxygen;
	he = 100 * eon->cache.gasmix[idx-1].helium;

	sample.event.type = SAMPLE_EVENT_GASCHANGE2;
	sample.event.value = o2 | (he << 16);

	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}

static int traverse_samples(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user)
{
	struct sample_data *info = (struct sample_data *) user;

	switch (type) {
	case 0x0001: // group: time in first word, depth in second
		sample_time(info, array_uint16_le(data));
		sample_depth(info, array_uint16_le(data+2));
		sample_temp(info, array_uint16_le(data+4));
		sample_deco(info, array_uint16_le(data+8), array_uint16_le(data+10), array_uint16_le(data+12));
		break;
	case 0x0002: // time in first word
		sample_time(info, array_uint16_le(data));
		break;
	case 0x0003: // depth in first word
		sample_depth(info, array_uint16_le(data));
		break;
	case 0x000a: // cylinder idx in first byte, pressure in next word
		sample_cylinder_pressure(info, data[0], array_uint16_le(data+1));
		break;
	case 0x001d:
		sample_gas_switch_event(info, array_uint16_le(data));
		break;
	}
	return 0;
}

static dc_status_t
suunto_eonsteel_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) abstract;
	struct sample_data data = { eon, callback, userdata, 0 };

	traverse_data(eon, traverse_samples, &data);
	return DC_STATUS_SUCCESS;
}

// Ugly define thing makes the code much easier to read
// I'd love to use __typeof__, but that's a gcc'ism
#define field_value(p, set) \
	memcpy((p), &(set), sizeof(set))

static dc_status_t
suunto_eonsteel_parser_get_field(dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *)parser;

	if (!(eon->cache.initialized >> type))
		return DC_STATUS_UNSUPPORTED;

	switch (type) {
	case DC_FIELD_DIVETIME:
		field_value(value, eon->cache.divetime);
		break;
	case DC_FIELD_MAXDEPTH:
		field_value(value, eon->cache.maxdepth);
		break;
	case DC_FIELD_AVGDEPTH:
		field_value(value, eon->cache.avgdepth);
		break;
	case DC_FIELD_GASMIX_COUNT:
		field_value(value, eon->cache.ngases);
		break;
	case DC_FIELD_GASMIX:
		if (flags >= MAXGASES)
			return DC_STATUS_UNSUPPORTED;
		field_value(value, eon->cache.gasmix[flags]);
		break;
	case DC_FIELD_SALINITY:
		field_value(value, eon->cache.salinity);
		break;
	case DC_FIELD_ATMOSPHERIC:
		field_value(value, eon->cache.surface_pressure);
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}
	return DC_STATUS_SUCCESS;
}

/*
 * The time of the dive is encoded in the filename,
 * and we've saved it off as the four first bytes
 * of the dive data (in little-endian format).
 */
static dc_status_t
suunto_eonsteel_parser_get_datetime(dc_parser_t *parser, dc_datetime_t *datetime)
{
	if (parser->size < 4)
		return DC_STATUS_UNSUPPORTED;

	dc_datetime_gmtime(datetime, array_uint32_le(parser->data));
	return DC_STATUS_SUCCESS;
}

// time in ms
static void add_time_field(suunto_eonsteel_parser_t *eon, unsigned short time_delta_ms)
{
	eon->cache.divetime += time_delta_ms;
}

// depth in cm
static void set_depth_field(suunto_eonsteel_parser_t *eon, unsigned short d)
{
	if (d != 0xffff) {
		double depth = d / 100.0;
		if (depth > eon->cache.maxdepth)
			eon->cache.maxdepth = depth;
		eon->cache.initialized |= 1 << DC_FIELD_MAXDEPTH;
	}
}

// gas type: 0=Off,1=Primary,2=?,3=Diluent
static void add_gas_type(suunto_eonsteel_parser_t *eon, unsigned char type)
{
	if (eon->cache.ngases < MAXGASES)
		eon->cache.ngases++;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX_COUNT;
}

// O2 percentage as a byte
static void add_gas_o2(suunto_eonsteel_parser_t *eon, unsigned char o2)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.gasmix[idx].oxygen = o2 / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
}

// He percentage as a byte
static void add_gas_he(suunto_eonsteel_parser_t *eon, unsigned char he)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.gasmix[idx].helium = he / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
}

static int traverse_fields(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) user;

	switch (type) {
	case 0x0001: // group: time in first word, depth in second
		add_time_field(eon, array_uint16_le(data));
		set_depth_field(eon, array_uint16_le(data+2));
		break;
	case 0x0002: // time in first word
		add_time_field(eon, array_uint16_le(data));
		break;
	case 0x0003: // depth in first word
		set_depth_field(eon, array_uint16_le(data));
		break;
	case 0x000d: // gas state in first byte
		add_gas_type(eon, data[0]);
		break;
	case 0x000e: // Oxygen percentage in first byte
		add_gas_o2(eon, data[0]);
		break;
	case 0x000f: // Helium percentage in first byte
		add_gas_he(eon, data[0]);
		break;
	}
	return 0;
}


static void initialize_field_caches(suunto_eonsteel_parser_t *eon)
{
	memset(&eon->cache, 0, sizeof(eon->cache));
	eon->cache.initialized = 1 << DC_FIELD_DIVETIME;

	traverse_data(eon, traverse_fields, eon);

	// The internal time fields are in ms and have to be added up
	// like that. At the end, we translate it back to seconds.
	eon->cache.divetime /= 1000;
}

static dc_status_t
suunto_eonsteel_parser_set_data(dc_parser_t *parser, const unsigned char *data, unsigned int size)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) parser;

	desc_free(eon->type_desc, MAXTYPE);
	memset(eon->type_desc, 0, sizeof(eon->type_desc));
	initialize_field_caches(eon);
	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_parser_destroy(dc_parser_t *parser)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) parser;

	desc_free(eon->type_desc, MAXTYPE);
	free(parser);
	return DC_STATUS_SUCCESS;
}

static const dc_parser_vtable_t suunto_eonsteel_parser_vtable = {
	DC_FAMILY_SUUNTO_EONSTEEL,
	suunto_eonsteel_parser_set_data, /* set_data */
	suunto_eonsteel_parser_get_datetime, /* datetime */
	suunto_eonsteel_parser_get_field, /* fields */
	suunto_eonsteel_parser_samples_foreach, /* samples_foreach */
	suunto_eonsteel_parser_destroy /* destroy */
};

dc_status_t
suunto_eonsteel_parser_create(dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	suunto_eonsteel_parser_t *eon;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	eon = (suunto_eonsteel_parser_t *) calloc(1, sizeof(*eon));
	if (!eon)
		return DC_STATUS_NOMEMORY;

	parser_init(&eon->base, context, &suunto_eonsteel_parser_vtable);

	*out = (dc_parser_t *) eon;

	return DC_STATUS_SUCCESS;
}
