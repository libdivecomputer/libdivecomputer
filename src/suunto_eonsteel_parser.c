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
#include <ctype.h>
#include <math.h>

#include <libdivecomputer/suunto_eonsteel.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"


#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

enum eon_sample {
	ES_none = 0,
	ES_dtime,	// duint16,precision=3 (time delta in ms)
	ES_depth,	// uint16,precision=2,nillable=65535 (depth in cm)
	ES_temp,	// int16,precision=2,nillable=-3000 (temp in deci-Celsius)
	ES_ndl,		// int16,nillable=-1 (ndl in minutes)
	ES_ceiling,	// uint16,precision=2,nillable=65535 (ceiling in cm)
	ES_tts,		// uint16,nillable=65535 (time to surface)
	ES_heading,	// uint16,precision=4,nillable=65535 (heading in degrees)
	ES_abspressure,	// uint16,precision=0,nillable=65535 (abs presure in centibar)
	ES_gastime,	// int16,nillable=-1 (remaining gas time in minutes)
	ES_ventilation,	// uint16,precision=6,nillable=65535 ("x/6000000,x"? No idea)
	ES_gasnr,	// uint8
	ES_pressure,	// uint16,nillable=65535 (cylinder pressure in centibar)
	ES_state,
	ES_state_active,
	ES_notify,
	ES_notify_active,
	ES_warning,
	ES_warning_active,
	ES_alarm,
	ES_alarm_active,
	ES_gasswitch,	// uint16
	ES_bookmark,
};

#define EON_MAX_GROUP 16

struct type_desc {
	const char *desc, *format, *mod;
	unsigned int size;
	enum eon_sample type[EON_MAX_GROUP];
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
		dc_tankvolume_t tankinfo[MAXGASES];
		double tanksize[MAXGASES];
		double tankworkingpressure[MAXGASES];
	} cache;
} suunto_eonsteel_parser_t;

typedef int (*eon_data_cb_t)(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user);

static const struct {
	const char *name;
	enum eon_sample type;
} type_translation[] = {
	{ "+Time",				ES_dtime },
	{ "Depth",				ES_depth },
	{ "Temperature",			ES_temp },
	{ "NoDecTime",				ES_ndl },
	{ "Ceiling",				ES_ceiling },
	{ "TimeToSurface",			ES_tts },
	{ "Heading",				ES_heading },
	{ "DeviceInternalAbsPressure",		ES_abspressure },
	{ "GasTime",				ES_gastime },
	{ "Ventilation",			ES_ventilation },
	{ "Cylinders+Cylinder.GasNumber",	ES_gasnr },
	{ "Cylinders.Cylinder.Pressure",	ES_pressure },
	{ "Events+State.Type",			ES_state },
	{ "Events.State.Active",		ES_state_active },
	{ "Events+Notify.Type",			ES_notify },
	{ "Events.Notify.Active",		ES_notify_active },
	{ "Events+Warning.Type",		ES_warning },
	{ "Events.Warning.Active",		ES_warning_active },
	{ "Events+Alarm.Type",			ES_alarm },
	{ "Events.Alarm.Active",		ES_alarm_active },
	{ "Events.Bookmark.Name",		ES_bookmark },
	{ "Events.GasSwitch.GasNumber",		ES_gasswitch },
	{ "Events.DiveTimer.Active",		ES_none },
	{ "Events.DiveTimer.Time",		ES_none },
};

static enum eon_sample lookup_descriptor_type(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	int i;
	const char *name = desc->desc;

	// Not a sample type? Skip it
	if (strncmp(name, "sml.DeviceLog.Samples", 21))
		return ES_none;

	// Skip the common base
	name += 21;

	// We have a "+Sample.Time", which starts a new
	// sample and contains the time delta
	if (!strcmp(name, "+Sample.Time"))
		return ES_dtime;

	// .. the rest should start with ".Sample."
	if (strncmp(name, ".Sample.", 8))
		return ES_none;

	// Skip the ".Sample."
	name += 8;

	// .. and look it up in the table of sample type strings
	for (i = 0; i < C_ARRAY_SIZE(type_translation); i++) {
		if (!strcmp(name, type_translation[i].name))
			return type_translation[i].type;
	}
	return ES_none;
}

static const char *desc_type_name(enum eon_sample type)
{
	int i;
	for (i = 0; i < C_ARRAY_SIZE(type_translation); i++) {
		if (type == type_translation[i].type)
			return type_translation[i].name;
	}
	return "Unknown";
}

static int lookup_descriptor_size(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	const char *format = desc->format;
	unsigned char c;

	if (!format)
		return 0;

	if (!strncmp(format, "bool", 4))
		return 1;
	if (!strncmp(format, "enum", 4))
		return 1;
	if (!strncmp(format, "utf8", 4))
		return 0;

	// find the byte size (eg "float32" -> 4 bytes)
	while ((c = *format) != 0) {
		if (isdigit(c))
			return atoi(format)/8;
		format++;
	}
	return 0;
}

static int fill_in_group_details(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	int subtype = 0;
	const char *grp = desc->desc;

	for (;;) {
		struct type_desc *base;
		char *end;
		long index;

		index = strtol(grp, &end, 10);
		if (index < 0 || index > MAXTYPE || end == grp) {
			ERROR(eon->base.context, "Group type descriptor '%s' does not parse", desc->desc);
			break;
		}
		base = eon->type_desc + index;
		if (!base->desc) {
			ERROR(eon->base.context, "Group type descriptor '%s' has undescribed index %d", desc->desc, index);
			break;
		}
		if (!base->size) {
			ERROR(eon->base.context, "Group type descriptor '%s' uses unsized sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (!base->type[0]) {
			ERROR(eon->base.context, "Group type descriptor '%s' has non-enumerated sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (base->type[1]) {
			ERROR(eon->base.context, "Group type descriptor '%s' has a recursive group sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (subtype >= EON_MAX_GROUP-1) {
			ERROR(eon->base.context, "Group type descriptor '%s' has too many sub-entries", desc->desc);
			break;
		}
		desc->size += base->size;
		desc->type[subtype++] = base->type[0];
		switch (*end) {
		case 0:
			return 0;
		case ',':
			grp = end+1;
			continue;
		default:
			ERROR(eon->base.context, "Group type descriptor '%s' has unparseable index %d", desc->desc, index);
			return -1;
		}
	}
	return -1;
}

/*
 * Here we cache descriptor data so that we don't have
 * to re-parse the string all the time. That way we can
 * do it just once per type.
 *
 * Right now we only bother with the sample descriptors,
 * which all start with "sml.DeviceLog.Samples" (for the
 * base types) or are "GRP" types that are a group of said
 * types and are a set of numbers.
 */
static int fill_in_desc_details(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	if (!desc->desc)
		return 0;

	if (isdigit(desc->desc[0]))
		return fill_in_group_details(eon, desc);

	desc->size = lookup_descriptor_size(eon, desc);
	desc->type[0] = lookup_descriptor_type(eon, desc);
	return 0;
}

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

	memset(&desc, 0, sizeof(desc));
	do {
		int len;
		char *p;

		next = strchr(name, '\n');
		if (next) {
			len = next - name;
			next++;
		} else {
			len = strlen(name);
			if (!len)
				break;
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

	fill_in_desc_details(eon, &desc);

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
	unsigned char state_type, notify_type;
	unsigned char warning_type, alarm_type;

	/* We gather up deco and cylinder pressure information */
	int gasnr;
	int tts, ndl;
	double ceiling;
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

static void sample_ndl(struct sample_data *info, short ndl)
{
	dc_sample_value_t sample = {0};

	info->ndl = ndl;
	if (ndl < 0)
		return;

	sample.deco.type = DC_DECO_NDL;
	sample.deco.time = ndl;
	if (info->callback) info->callback(DC_SAMPLE_DECO, sample, info->userdata);
}

static void sample_tts(struct sample_data *info, unsigned short tts)
{
	if (tts != 0xffff)
		info->tts = tts;
}

static void sample_ceiling(struct sample_data *info, unsigned short ceiling)
{
	if (ceiling != 0xffff)
		info->ceiling = ceiling / 100.0;
}

static void sample_heading(struct sample_data *info, unsigned short heading)
{
	dc_sample_value_t sample = {0};

	if (heading == 0xffff)
		return;

	sample.event.type = SAMPLE_EVENT_HEADING;
	sample.event.value = heading;
	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}

static void sample_abspressure(struct sample_data *info, unsigned short pressure)
{
}

static void sample_gastime(struct sample_data *info, short gastime)
{
	dc_sample_value_t sample = {0};

	if (gastime < 0)
		return;

	// Hmm. We have no good way to report airtime remaining
}

/*
 * Per-sample "ventilation" data.
 *
 * It's described as:
 *   - "uint16,precision=6,nillable=65535"
 *   - "x/6000000,x"
 */
static void sample_ventilation(struct sample_data *info, unsigned short unk)
{
}

static void sample_gasnr(struct sample_data *info, unsigned char idx)
{
	info->gasnr = idx;
}

static void sample_pressure(struct sample_data *info, unsigned short pressure)
{
	dc_sample_value_t sample = {0};

	if (pressure == 0xffff)
		return;

	sample.pressure.tank = info->gasnr-1;
	sample.pressure.value = pressure / 100.0;
	if (info->callback) info->callback(DC_SAMPLE_PRESSURE, sample, info->userdata);
}

static void sample_bookmark_event(struct sample_data *info, unsigned short idx)
{
	dc_sample_value_t sample = {0};

	sample.event.type = SAMPLE_EVENT_BOOKMARK;
	sample.event.value = idx;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}

static void sample_gas_switch_event(struct sample_data *info, unsigned short idx)
{
	suunto_eonsteel_parser_t *eon = info->eon;
	dc_sample_value_t sample = {0};

	if (idx < 1 || idx > eon->cache.ngases)
		return;

	sample.gasmix = idx - 1;
	if (info->callback) info->callback(DC_SAMPLE_GASMIX, sample, info->userdata);

#ifdef ENABLE_DEPRECATED
	unsigned int o2 = 100 * eon->cache.gasmix[idx-1].oxygen;
	unsigned int he = 100 * eon->cache.gasmix[idx-1].helium;
	sample.event.type = SAMPLE_EVENT_GASCHANGE2;
	sample.event.time = 0;
	sample.event.flags = 0;
	sample.event.value = o2 | (he << 16);
	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
#endif
}

/*
 * The EON Steel has four different sample events: "state", "notification",
 * "warning" and "alarm". All end up having two fields: type and a boolean value.
 *
 * The type enumerations are available as part of the type descriptor, and we
 * *should* probably parse them dynamically, but this hardcodes the different
 * type values.
 *
 * For event states, the types are:
 *
 * 0=Wet Outside
 * 1=Below Wet Activation Depth
 * 2=Below Surface
 * 3=Dive Active
 * 4=Surface Calculation
 * 5=Tank pressure available
 *
 * FIXME! This needs to parse the actual type descriptor enum
 */
static void sample_event_state_type(struct sample_data *info, unsigned char type)
{
	info->state_type = type;
}

static void sample_event_state_value(struct sample_data *info, unsigned char value)
{
	/*
	 * We could turn these into sample events, but they don't actually
	 * match any libdivecomputer events.
	 *
	 *   unsigned int state = info->state_type;
	 *   dc_sample_value_t sample = {0};
	 *   sample.event.type = ...
	 *   sample.event.value = value;
	 *   if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
	 */
}

static void sample_event_notify_type(struct sample_data *info, unsigned char type)
{
	info->notify_type = type;
}


// FIXME! This needs to parse the actual type descriptor enum
static void sample_event_notify_value(struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	static const enum parser_sample_event_t translate_notification[] = {
		SAMPLE_EVENT_NONE,			// 0=NoFly Time
		SAMPLE_EVENT_NONE,			// 1=Depth
		SAMPLE_EVENT_NONE,			// 2=Surface Time
		SAMPLE_EVENT_TISSUELEVEL,		// 3=Tissue Level
		SAMPLE_EVENT_NONE,			// 4=Deco
		SAMPLE_EVENT_NONE,			// 5=Deco Window
		SAMPLE_EVENT_SAFETYSTOP_VOLUNTARY,	// 6=Safety Stop Ahead
		SAMPLE_EVENT_SAFETYSTOP,		// 7=Safety Stop
		SAMPLE_EVENT_CEILING_SAFETYSTOP,	// 8=Safety Stop Broken
		SAMPLE_EVENT_NONE,			// 9=Deep Stop Ahead
		SAMPLE_EVENT_DEEPSTOP,			// 10=Deep Stop
		SAMPLE_EVENT_DIVETIME,			// 11=Dive Time
		SAMPLE_EVENT_NONE,			// 12=Gas Available
		SAMPLE_EVENT_NONE,			// 13=SetPoint Switch
		SAMPLE_EVENT_NONE,			// 14=Diluent Hypoxia
		SAMPLE_EVENT_NONE,			// 15=Tank Pressure
	};

	if (info->notify_type > 15)
		return;

	sample.event.type = translate_notification[info->notify_type];
	if (sample.event.type == SAMPLE_EVENT_NONE)
		return;

	sample.event.value = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}


static void sample_event_warning_type(struct sample_data *info, unsigned char type)
{
	info->warning_type = type;
}


static void sample_event_warning_value(struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	static const enum parser_sample_event_t translate_warning[] = {
		SAMPLE_EVENT_NONE,			// 0=ICD Penalty ("Isobaric counterdiffusion")
		SAMPLE_EVENT_VIOLATION,			// 1=Deep Stop Penalty
		SAMPLE_EVENT_SAFETYSTOP_MANDATORY,	// 2=Mandatory Safety Stop
		SAMPLE_EVENT_NONE,			// 3=OTU250
		SAMPLE_EVENT_NONE,			// 4=OTU300
		SAMPLE_EVENT_NONE,			// 5=CNS80%
		SAMPLE_EVENT_NONE,			// 6=CNS100%
		SAMPLE_EVENT_AIRTIME,			// 7=Air Time
		SAMPLE_EVENT_MAXDEPTH,			// 8=Max.Depth
		SAMPLE_EVENT_AIRTIME,			// 9=Tank Pressure
		SAMPLE_EVENT_CEILING_SAFETYSTOP,	// 10=Safety Stop Broken
		SAMPLE_EVENT_CEILING_SAFETYSTOP,	// 11=Deep Stop Broken
		SAMPLE_EVENT_CEILING,			// 12=Ceiling Broken
		SAMPLE_EVENT_PO2,			// 13=PO2 High
	};

	if (info->warning_type > 13)
		return;

	sample.event.type = translate_warning[info->warning_type];
	if (sample.event.type == SAMPLE_EVENT_NONE)
		return;

	sample.event.value = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}

static void sample_event_alarm_type(struct sample_data *info, unsigned char type)
{
	info->alarm_type = type;
}


// FIXME! This needs to parse the actual type descriptor enum
static void sample_event_alarm_value(struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	static const enum parser_sample_event_t translate_alarm[] = {
		SAMPLE_EVENT_CEILING_SAFETYSTOP,	// 0=Mandatory Safety Stop Broken
		SAMPLE_EVENT_ASCENT,			// 1=Ascent Speed
		SAMPLE_EVENT_NONE,			// 2=Diluent Hyperoxia
		SAMPLE_EVENT_VIOLATION,			// 3=Violated Deep Stop
		SAMPLE_EVENT_CEILING,			// 4=Ceiling Broken
		SAMPLE_EVENT_PO2,			// 5=PO2 High
		SAMPLE_EVENT_PO2,			// 6=PO2 Low
	};

	if (info->alarm_type > 6)
		return;

	sample.event.type = translate_alarm[info->alarm_type];
	if (sample.event.type == SAMPLE_EVENT_NONE)
		return;

	sample.event.value = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	if (info->callback) info->callback(DC_SAMPLE_EVENT, sample, info->userdata);
}

static int handle_sample_type(struct sample_data *info, enum eon_sample type, const unsigned char *data)
{
	switch (type) {
	case ES_dtime:
		sample_time(info, array_uint16_le(data));
		return 2;

	case ES_depth:
		sample_depth(info, array_uint16_le(data));
		return 2;

	case ES_temp:
		sample_temp(info, array_uint16_le(data));
		return 2;

	case ES_ndl:
		sample_ndl(info, array_uint16_le(data));
		return 2;

	case ES_ceiling:
		sample_ceiling(info, array_uint16_le(data));
		return 2;

	case ES_tts:
		sample_tts(info, array_uint16_le(data));
		return 2;

	case ES_heading:
		sample_heading(info, array_uint16_le(data));
		return 2;

	case ES_abspressure:
		sample_abspressure(info, array_uint16_le(data));
		return 2;

	case ES_gastime:
		sample_gastime(info, array_uint16_le(data));
		return 2;

	case ES_ventilation:
		sample_ventilation(info, array_uint16_le(data));
		return 2;

	case ES_gasnr:
		sample_gasnr(info, *data);
		return 1;

	case ES_pressure:
		sample_pressure(info, array_uint16_le(data));
		return 2;

	case ES_state:
		sample_event_state_type(info, data[0]);
		return 1;

	case ES_state_active:
		sample_event_state_value(info, data[0]);
		return 1;

	case ES_notify:
		sample_event_notify_type(info, data[0]);
		return 1;

	case ES_notify_active:
		sample_event_notify_value(info, data[0]);
		return 1;

	case ES_warning:
		sample_event_warning_type(info, data[0]);
		return 1;

	case ES_warning_active:
		sample_event_warning_value(info, data[0]);
		return 1;

	case ES_alarm:
		sample_event_alarm_type(info, data[0]);
		return 1;

	case ES_alarm_active:
		sample_event_alarm_value(info, data[0]);
		return 1;

	case ES_bookmark:
		sample_bookmark_event(info, array_uint16_le(data));
		return 2;

	case ES_gasswitch:
		sample_gas_switch_event(info, array_uint16_le(data));
		return 2;

	default:
		return 0;
	}
}

static int traverse_samples(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user)
{
	struct sample_data *info = (struct sample_data *) user;
	suunto_eonsteel_parser_t *eon = info->eon;
	int i, used = 0;

	if (desc->size > len)
		ERROR(eon->base.context, "Got %d bytes of data for '%s' that wants %d bytes", len, desc->desc, desc->size);

	info->ndl = -1;
	info->tts = 0;
	info->ceiling = 0.0;

	for (i = 0; i < EON_MAX_GROUP; i++) {
		enum eon_sample type = desc->type[i];
		int bytes = handle_sample_type(info, type, data);

		if (!bytes)
			break;
		if (bytes > len) {
			ERROR(eon->base.context, "Wanted %d bytes of data, only had %d bytes ('%s' idx %d)", bytes, len, desc->desc, i);
			break;
		}
		data += bytes;
		len -= bytes;
		used += bytes;
	}

	if (info->ndl < 0 && (info->tts || info->ceiling)) {
		dc_sample_value_t sample = {0};

		sample.deco.type = DC_DECO_DECOSTOP;
		sample.deco.time = info->tts;
		sample.deco.depth = info->ceiling;
		if (info->callback) info->callback(DC_SAMPLE_DECO, sample, info->userdata);
	}

	// Warn if there are left-over bytes for something we did use part of
	if (used && len)
		ERROR(eon->base.context, "Entry for '%s' had %d bytes, only used %d", desc->desc, len+used, used);
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
	dc_tank_t *tank = (dc_tank_t *) value;

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
	case DC_FIELD_TANK_COUNT:
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
	case DC_FIELD_TANK:
		/*
		 * Sadly it seems that the EON Steel doesn't tell us whether
		 * we get imperial or metric data - the only indication is
		 * that metric is (at least so far) always whole liters
		 */
		tank->volume = eon->cache.tanksize[flags];

		/*
		 * The pressure reported is NOT the pressure the user enters.
		 *
		 * So 3000psi turns into 206.700 bar instead of 206.843 bar;
		 * We report it as we get it and let the application figure out
		 * what to do with that
		 */
		tank->workpressure = eon->cache.tankworkingpressure[flags];
		tank->type = eon->cache.tankinfo[flags];

		/*
		 * See if we should call this imperial instead.
		 *
		 * We need to have workpressure and a valid tank. In that case,
		 * a fractional tank size implies imperial.
		 */
		if (tank->workpressure && (tank->type == DC_TANKVOLUME_METRIC)) {
			if (fabs(tank->volume - rint(tank->volume)) > 0.001)
				tank->type = DC_TANKVOLUME_IMPERIAL;
		}
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

// new gas:
//  "sml.DeviceLog.Header.Diving.Gases+Gas.State"
//
// We eventually need to parse the descriptor for that 'enum type'.
// Two versions so far:
//   "enum:0=Off,1=Primary,2=?,3=Diluent"
//   "enum:0=Off,1=Primary,3=Diluent,4=Oxygen"
//
// We turn that into the DC_TANKVOLUME data here, but
// initially consider all non-off tanks to me METRIC.
//
// We may later turn the METRIC tank size into IMPERIAL if we
// get a working pressure and non-integral size
static int add_gas_type(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, unsigned char type)
{
	int idx = eon->cache.ngases;
	dc_tankvolume_t tankinfo = DC_TANKVOLUME_METRIC;

	if (idx >= MAXGASES)
		return 0;

	eon->cache.ngases = idx+1;
	switch (type) {
	case 0:
		tankinfo = 0;
		break;
	default:
		break;
	}
	eon->cache.tankinfo[idx] = tankinfo;

	eon->cache.initialized |= 1 << DC_FIELD_GASMIX_COUNT;
	return 0;
}

// "sml.DeviceLog.Header.Diving.Gases.Gas.Oxygen"
// O2 percentage as a byte
static int add_gas_o2(suunto_eonsteel_parser_t *eon, unsigned char o2)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.gasmix[idx].oxygen = o2 / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
	return 0;
}

// "sml.DeviceLog.Header.Diving.Gases.Gas.Helium"
// He percentage as a byte
static int add_gas_he(suunto_eonsteel_parser_t *eon, unsigned char he)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.gasmix[idx].helium = he / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
	return 0;
}

static int add_gas_size(suunto_eonsteel_parser_t *eon, float l)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.tanksize[idx] = l;
	return 0;
}

static int add_gas_workpressure(suunto_eonsteel_parser_t *eon, float wp)
{
	int idx = eon->cache.ngases-1;
	if (idx >= 0)
		eon->cache.tankworkingpressure[idx] = wp;
	return 0;
}

static float get_le32_float(const unsigned char *src)
{
	union {
		unsigned int val;
		float result;
	} u;

	u.val = array_uint32_le(src);
	return u.result;
}

// "Device" fields are all utf8:
//   Info.BatteryAtEnd
//   Info.BatteryAtStart
//   Info.BSL
//   Info.HW
//   Info.SW
//   Name
//   SerialNumber
static int traverse_device_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc,
                                  const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Device.");

	return 0;
}

// "sml.DeviceLog.Header.Diving."
//
//   Gases+Gas.State (enum:0=Off,1=Primary,3=Diluent,4=Oxygen)
//   Gases.Gas.Oxygen (uint8,precision=2)
//   Gases.Gas.Helium (uint8,precision=2)
//   Gases.Gas.PO2 (uint32)
//   Gases.Gas.TransmitterID (utf8)
//   Gases.Gas.TankSize (float32,precision=5)
//   Gases.Gas.TankFillPressure (float32,precision=0)
//   Gases.Gas.StartPressure (float32,precision=0)
//   Gases.Gas.EndPressure (float32,precision=0)
//   Gases.Gas.TransmitterStartBatteryCharge (int8,precision=2)
//   Gases.Gas.TransmitterEndBatteryCharge (int8,precision=2)
//   SurfaceTime (uint32)
//   NumberInSeries (uint32)
//   Algorithm (utf8)
//   SurfacePressure (uint32)
//   Conservatism (int8)
//   Altitude (uint16)
//   AlgorithmTransitionDepth (uint8)
//   DaysInSeries (uint32)
//   PreviousDiveDepth (float32,precision=2)
//   StartTissue.CNS (float32,precision=3)
//   StartTissue.OTU (float32)
//   StartTissue.OLF (float32,precision=3)
//   StartTissue.Nitrogen+Pressure (uint32)
//   StartTissue.Helium+Pressure (uint32)
//   StartTissue.RgbmNitrogen (float32,precision=3)
//   StartTissue.RgbmHelium (float32,precision=3)
//   DiveMode (utf8)
//   AlgorithmBottomTime (uint32)
//   AlgorithmAscentTime (uint32)
//   AlgorithmBottomMixture.Oxygen (uint8,precision=2)
//   AlgorithmBottomMixture.Helium (uint8,precision=2)
//   DesaturationTime (uint32)
//   EndTissue.CNS (float32,precision=3)
//   EndTissue.OTU (float32)
//   EndTissue.OLF (float32,precision=3)
//   EndTissue.Nitrogen+Pressure (uint32)
//   EndTissue.Helium+Pressure (uint32)
//   EndTissue.RgbmNitrogen (float32,precision=3)
//   EndTissue.RgbmHelium (float32,precision=3)
static int traverse_diving_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc,
                                  const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Header.Diving.");

	if (!strcmp(name, "Gases+Gas.State"))
		return add_gas_type(eon, desc, data[0]);

	if (!strcmp(name, "Gases.Gas.Oxygen"))
		return add_gas_o2(eon, data[0]);

	if (!strcmp(name, "Gases.Gas.Helium"))
		return add_gas_he(eon, data[0]);

	if (!strcmp(name, "Gases.Gas.TankSize"))
		return add_gas_size(eon, get_le32_float(data));

	if (!strcmp(name, "Gases.Gas.TankFillPressure"))
		return add_gas_workpressure(eon, get_le32_float(data));

	if (!strcmp(name, "SurfacePressure")) {
		unsigned int pressure = array_uint32_le(data); // in SI units - Pascal
		eon->cache.surface_pressure = pressure / 100000.0; // bar
		eon->cache.initialized |= 1 << DC_FIELD_ATMOSPHERIC;
		return 0;
	}

	return 0;
}

// "Header" fields are:
//   Activity (utf8)
//   DateTime (utf8)
//   Depth.Avg (float32,precision=2)
//   Depth.Max (float32,precision=2)
//   Diving.*
//   Duration (uint32)
//   PauseDuration (uint32)
//   SampleInterval (uint8)
static int traverse_header_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc,
                                  const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Header.");

	if (!strncmp(name, "Diving.", 7))
		return traverse_diving_fields(eon, desc, data, len);

	if (!strcmp(name, "Depth.Max")) {
		double d = get_le32_float(data);
		if (d > eon->cache.maxdepth)
			eon->cache.maxdepth = d;
		return 0;
	}

	return 0;
}

static int traverse_dynamic_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, const unsigned char *data, int len)
{
	const char *name = desc->desc;

	if (!strncmp(name, "sml.", 4)) {
		name += 4;
		if (!strncmp(name, "DeviceLog.", 10)) {
			name += 10;
			if (!strncmp(name, "Device.", 7))
				return traverse_device_fields(eon, desc, data, len);
			if (!strncmp(name, "Header.", 7)) {
				return traverse_header_fields(eon, desc, data, len);
			}
		}
	}
	return 0;
}

/*
 * This is a simplified sample parser that only parses the depth and time
 * samples. It also depends on the GRP entries always starting with time/depth,
 * and just stops on anything else.
 */
static int traverse_sample_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, const unsigned char *data, int len)
{
	int i;

	for (i = 0; i < EON_MAX_GROUP; i++) {
		enum eon_sample type = desc->type[i];

		switch (type) {
		case ES_dtime:
			add_time_field(eon, array_uint16_le(data));
			data += 2;
			continue;
		case ES_depth:
			set_depth_field(eon, array_uint16_le(data));
			data += 2;
			continue;
		}
		break;
	}
	return 0;
}

static int traverse_fields(unsigned short type, const struct type_desc *desc, const unsigned char *data, int len, void *user)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) user;

	// Sample type? Do basic maxdepth and time parsing
	if (desc->type[0])
		traverse_sample_fields(eon, desc, data, len);
	else
		traverse_dynamic_fields(eon, desc, data, len);

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

static void show_descriptor(suunto_eonsteel_parser_t *eon, int nr, struct type_desc *desc)
{
	int i;

	if (!desc->desc)
		return;
	DEBUG(eon->base.context, "Descriptor %d: '%s', size %d bytes", nr, desc->desc, desc->size);
	if (desc->format)
		DEBUG(eon->base.context, "    format '%s'", desc->format);
	if (desc->mod)
		DEBUG(eon->base.context, "    mod '%s'", desc->mod);
	for (i = 0; i < EON_MAX_GROUP; i++) {
		enum eon_sample type = desc->type[i];
		if (!type)
			continue;
		DEBUG(eon->base.context, "    %d: %d (%s)", i, type, desc_type_name(type));
	}
}

static void show_all_descriptors(suunto_eonsteel_parser_t *eon)
{
	for (unsigned int i = 0; i < MAXTYPE; ++i)
		show_descriptor(eon, i, eon->type_desc+i);
}

static dc_status_t
suunto_eonsteel_parser_set_data(dc_parser_t *parser, const unsigned char *data, unsigned int size)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) parser;

	desc_free(eon->type_desc, MAXTYPE);
	memset(eon->type_desc, 0, sizeof(eon->type_desc));
	initialize_field_caches(eon);
	show_all_descriptors(eon);
	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_parser_destroy(dc_parser_t *parser)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) parser;

	desc_free(eon->type_desc, MAXTYPE);

	return DC_STATUS_SUCCESS;
}

static const dc_parser_vtable_t suunto_eonsteel_parser_vtable = {
	sizeof(suunto_eonsteel_parser_t),
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
	suunto_eonsteel_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	parser = (suunto_eonsteel_parser_t *) dc_parser_allocate (context, &suunto_eonsteel_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	memset(&parser->type_desc, 0, sizeof(parser->type_desc));
	memset(&parser->cache, 0, sizeof(parser->cache));

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}
