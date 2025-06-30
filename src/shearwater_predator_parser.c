/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#include <libdivecomputer/units.h>

#include "shearwater_predator.h"
#include "shearwater_petrel.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"
#include "array.h"

#define ISINSTANCE(parser)	( \
	dc_parser_isinstance((parser), &shearwater_predator_parser_vtable) || \
	dc_parser_isinstance((parser), &shearwater_petrel_parser_vtable))

#define LOG_RECORD_DIVE_SAMPLE     0x01
#define LOG_RECORD_FREEDIVE_SAMPLE 0x02
#define LOG_RECORD_OPENING_0       0x10
#define LOG_RECORD_OPENING_1       0x11
#define LOG_RECORD_OPENING_2       0x12
#define LOG_RECORD_OPENING_3       0x13
#define LOG_RECORD_OPENING_4       0x14
#define LOG_RECORD_OPENING_5       0x15
#define LOG_RECORD_OPENING_6       0x16
#define LOG_RECORD_OPENING_7       0x17
#define LOG_RECORD_CLOSING_0       0x20
#define LOG_RECORD_CLOSING_1       0x21
#define LOG_RECORD_CLOSING_2       0x22
#define LOG_RECORD_CLOSING_3       0x23
#define LOG_RECORD_CLOSING_4       0x24
#define LOG_RECORD_CLOSING_5       0x25
#define LOG_RECORD_CLOSING_6       0x26
#define LOG_RECORD_CLOSING_7       0x27
#define LOG_RECORD_INFO_EVENT      0x30
#define LOG_RECORD_DIVE_SAMPLE_EXT 0xE1
#define LOG_RECORD_FINAL           0xFF

#define INFO_EVENT_TAG_LOG         38

#define SZ_BLOCK   0x80
#define SZ_SAMPLE_PREDATOR  0x10
#define SZ_SAMPLE_PETREL    0x20
#define SZ_SAMPLE_FREEDIVE  0x08

#define GASSWITCH     0x01
#define PPO2_EXTERNAL 0x02
#define SETPOINT_HIGH 0x04
#define SC            0x08
#define OC            0x10

#define M_CC       0
#define M_OC_TEC   1
#define M_GAUGE    2
#define M_PPO2     3
#define M_SC       4
#define M_CC2      5
#define M_OC_REC   6
#define M_FREEDIVE 7

#define AI_OFF   0
#define AI_HPCCR 4
#define AI_ON    5

#define GF       0
#define VPMB     1
#define VPMB_GFS 2
#define DCIEM    3

#define METRIC   0
#define IMPERIAL 1

#define NGASMIXES 20
#define NFIXED    10
#define NTANKS    6
#define NRECORDS  8

#define PREDATOR 2
#define PETREL   3
#define TERIC    8

#define UNDEFINED 0xFFFFFFFF

typedef struct shearwater_predator_parser_t shearwater_predator_parser_t;

typedef struct shearwater_predator_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
	unsigned int diluent;
	unsigned int enabled;
	unsigned int active;
} shearwater_predator_gasmix_t;

typedef struct shearwater_predator_tank_t {
	unsigned int enabled;
	unsigned int active;
	unsigned int beginpressure;
	unsigned int endpressure;
	unsigned int pressure_max;
	unsigned int pressure_reserve;
	unsigned int serial;
	char name[2];
	dc_usage_t usage;
} shearwater_predator_tank_t;

struct shearwater_predator_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int petrel;
	unsigned int samplesize;
	// Cached fields.
	unsigned int cached;
	unsigned int pnf;
	unsigned int logversion;
	unsigned int headersize;
	unsigned int footersize;
	unsigned int opening[NRECORDS];
	unsigned int closing[NRECORDS];
	unsigned int final;
	unsigned int ngasmixes;
	unsigned int ntanks;
	shearwater_predator_gasmix_t gasmix[NGASMIXES];
	shearwater_predator_tank_t tank[NTANKS];
	unsigned int tankidx[NTANKS];
	unsigned int aimode;
	unsigned int hpccr;
	unsigned int calibrated;
	double calibration[3];
	unsigned int divemode;
	unsigned int units;
	unsigned int atmospheric;
	unsigned int density;
};

static dc_status_t shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static dc_status_t shearwater_predator_parser_cache (shearwater_predator_parser_t *parser);

static const dc_parser_vtable_t shearwater_predator_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PREDATOR,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const dc_parser_vtable_t shearwater_petrel_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PETREL,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


static unsigned int
shearwater_predator_is_ccr (unsigned int divemode)
{
	return divemode == M_CC || divemode == M_CC2 || divemode == M_SC;
}

static unsigned int
shearwater_predator_find_gasmix (shearwater_predator_parser_t *parser, unsigned int o2, unsigned int he, unsigned int dil)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->gasmix[i].oxygen && he == parser->gasmix[i].helium && dil == parser->gasmix[i].diluent)
			break;
		i++;
	}

	return i;
}


static dc_status_t
shearwater_common_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model, unsigned int petrel)
{
	shearwater_predator_parser_t *parser = NULL;
	const dc_parser_vtable_t *vtable = NULL;
	unsigned int samplesize = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (petrel) {
		vtable = &shearwater_petrel_parser_vtable;
		samplesize = SZ_SAMPLE_PETREL;
	} else {
		vtable = &shearwater_predator_parser_vtable;
		samplesize = SZ_SAMPLE_PREDATOR;
	}

	// Allocate memory.
	parser = (shearwater_predator_parser_t *) dc_parser_allocate (context, vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->petrel = petrel;
	parser->samplesize = samplesize;
	parser->cached = 0;
	parser->pnf = 0;
	parser->logversion = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	for (unsigned int i = 0; i < NRECORDS; ++i) {
		parser->opening[i] = UNDEFINED;
		parser->closing[i] = UNDEFINED;
	}
	parser->final = UNDEFINED;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
		parser->gasmix[i].diluent = 0;
		parser->gasmix[i].enabled = 0;
		parser->gasmix[i].active = 0;
	}
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].enabled = 0;
		parser->tank[i].active = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tank[i].pressure_max = 0;
		parser->tank[i].pressure_reserve = 0;
		parser->tank[i].serial = 0;
		memset (parser->tank[i].name, 0, sizeof (parser->tank[i].name));
		parser->tank[i].usage = DC_USAGE_NONE;
		parser->tankidx[i] = i;
	}
	parser->aimode = AI_OFF;
	parser->hpccr = 0;
	parser->calibrated = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		parser->calibration[i] = 0.0;
	}
	parser->divemode = M_OC_TEC;
	parser->units = METRIC;
	parser->density = DEF_DENSITY_SALT;
	parser->atmospheric = DEF_ATMOSPHERIC / (BAR / 1000);

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	return shearwater_common_parser_create (out, context, data, size, model, 0);
}


dc_status_t
shearwater_petrel_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	return shearwater_common_parser_create (out, context, data, size, model, 1);
}


static dc_status_t
shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int ticks = array_uint32_be (data + parser->opening[0] + 12);

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	if (parser->model == TERIC && parser->logversion >= 9 && parser->opening[5] != UNDEFINED) {
		int utc_offset = (int) array_uint32_be (data + parser->opening[5] + 26);
		int dst = data[parser->opening[5] + 30];
		datetime->timezone = utc_offset * 60 + dst * 3600;
	} else {
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_cache (shearwater_predator_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	// Log versions before 6 weren't reliably stored in the data, but
	// 6 is also the oldest version that we assume in our code
	unsigned int logversion = 0;

	// Verify the minimum length.
	if (size < 2) {
		ERROR (abstract->context, "Invalid data length.");
		return DC_STATUS_DATAFORMAT;
	}

	// The Petrel Native Format (PNF) is very similar to the legacy
	// Predator and Predator-like format. The samples are simply offset
	// by one (so we can use pnf as the offset). For the header and
	// footer data, it's more complicated because of the new 32 byte
	// block structure.
	unsigned int pnf = parser->petrel ? array_uint16_be (data) != 0xFFFF : 0;
	unsigned int headersize = 0;
	unsigned int footersize = 0;
	if (!pnf) {
		// Opening and closing blocks.
		headersize = SZ_BLOCK;
		footersize = SZ_BLOCK;
		if (size < headersize + footersize) {
			ERROR (abstract->context, "Invalid data length.");
			return DC_STATUS_DATAFORMAT;
		}

		// Adjust the footersize for the final block.
		if (parser->petrel || array_uint16_be (data + size - footersize) == 0xFFFD) {
			footersize += SZ_BLOCK;
			if (size < headersize + footersize) {
				ERROR (abstract->context, "Invalid data length.");
				return DC_STATUS_DATAFORMAT;
			}

			parser->final = size - SZ_BLOCK;
		}

		// The Predator and Predator-like format have just one large 128
		// byte opening and closing block. To minimize the differences
		// with the PNF format, all record offsets are assigned the same
		// value here.
		for (unsigned int i = 0; i <= 4; ++i) {
			parser->opening[i] = 0;
			parser->closing[i] = size - footersize;
		}

		// Log version
		logversion = data[127];
	}

	// Default dive mode.
	unsigned int divemode = M_OC_TEC;

	// Get the gas mixes.
	unsigned int ngasmixes = NFIXED;
	shearwater_predator_gasmix_t gasmix[NGASMIXES] = {0};
	shearwater_predator_tank_t tank[NTANKS] = {0};
	unsigned int o2_previous = UNDEFINED, he_previous = UNDEFINED, dil_previous = UNDEFINED;
	unsigned int aimode = AI_OFF;
	unsigned int hpccr = 0;
	if (!pnf) {
		for (unsigned int i = 0; i < NFIXED; ++i) {
			gasmix[i].oxygen = data[20 + i];
			gasmix[i].helium = data[30 + i];
			gasmix[i].diluent = i >= 5;
			gasmix[i].enabled = 1;
		}
	}

	unsigned int offset = headersize;
	unsigned int length = size - footersize;
	while (offset + parser->samplesize <= length) {
		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Get the record type.
		unsigned int type = pnf ? data[offset] : LOG_RECORD_DIVE_SAMPLE;

		if (type == LOG_RECORD_DIVE_SAMPLE) {
			// Status flags.
			unsigned int status = data[offset + 11 + pnf];
			unsigned int ccr = (status & OC) == 0;
			if (ccr) {
				divemode = status & SC ? M_SC : M_CC;
			}

			// Gaschange.
			unsigned int o2 = data[offset + 7 + pnf];
			unsigned int he = data[offset + 8 + pnf];
			if ((o2 != o2_previous || he != he_previous || ccr != dil_previous) &&
				(o2 != 0 || he != 0)) {
				// Find the gasmix in the list.
				unsigned int idx = 0;
				while (idx < ngasmixes) {
					if (o2 == gasmix[idx].oxygen && he == gasmix[idx].helium && ccr == gasmix[idx].diluent)
						break;
					idx++;
				}

				// Add it to list if not found.
				if (idx >= ngasmixes) {
					if (idx >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					gasmix[idx].oxygen = o2;
					gasmix[idx].helium = he;
					gasmix[idx].diluent = ccr;
					ngasmixes = idx + 1;
				}

				gasmix[idx].active = 1;

				o2_previous = o2;
				he_previous = he;
				dil_previous = ccr;
			}

			// Tank pressure
			if (logversion >= 7) {
				const unsigned int idx[2] = {27, 19};
				for (unsigned int i = 0; i < 2; ++i) {
					// Values above 0xFFF0 are special codes:
					//    0xFFFF AI is off
					//    0xFFFE No comms for 90 seconds+
					//    0xFFFD No comms for 30 seconds
					//    0xFFFC Transmitter not paired
					// For regular values, the top 4 bits contain the battery
					// level (0=normal, 1=critical, 2=warning), and the lower 12
					// bits the tank pressure in units of 2 psi.
					unsigned int pressure = array_uint16_be (data + offset + pnf + idx[i]);
					unsigned int id = (aimode == AI_HPCCR ? 4 : 0) + i;
					if (pressure < 0xFFF0) {
						pressure &= 0x0FFF;
						if (pressure) {
							if (!tank[id].active) {
								tank[id].active = 1;
								tank[id].beginpressure = pressure;
								tank[id].endpressure = pressure;
							}
							tank[id].endpressure = pressure;
						}
					}
				}
			}
		} else if (type == LOG_RECORD_DIVE_SAMPLE_EXT) {
			// Tank pressure
			if (logversion >= 13) {
				for (unsigned int i = 0; i < 2; ++i) {
					unsigned int pressure = array_uint16_be (data + offset + pnf + i * 2);
					unsigned int id = 2 + i;
					if (pressure < 0xFFF0) {
						pressure &= 0x0FFF;
						if (pressure) {
							if (!tank[id].active) {
								tank[id].active = 1;
								tank[id].beginpressure = pressure;
								tank[id].endpressure = pressure;
							}
							tank[id].endpressure = pressure;
						}
					}
				}
			}
			// Tank pressure (HP CCR)
			if (logversion >= 14) {
				for (unsigned int i = 0; i < 2; ++i) {
					unsigned int pressure = array_uint16_be (data + offset + pnf + 4 + i * 2);
					unsigned int id = 4 + i;
					if (pressure) {
						if (!tank[id].active) {
							tank[id].active = 1;
							tank[id].enabled = 1;
							tank[id].beginpressure = pressure;
							tank[id].endpressure = pressure;
							tank[id].usage = i == 0 ? DC_USAGE_DILUENT : DC_USAGE_OXYGEN;
							hpccr = 1;
						}
						tank[id].endpressure = pressure;
					}
				}
			}
		} else if (type == LOG_RECORD_FREEDIVE_SAMPLE) {
			// Freedive record
			divemode = M_FREEDIVE;
		} else if (type >= LOG_RECORD_OPENING_0 && type <= LOG_RECORD_OPENING_7) {
			// Opening record
			parser->opening[type - LOG_RECORD_OPENING_0] = offset;

			if (type == LOG_RECORD_OPENING_0) {
				for (unsigned int i = 0; i < NFIXED; ++i) {
					gasmix[i].oxygen = data[offset + 20 + i];
					gasmix[i].diluent = i >= 5;
				}
				for (unsigned int i = 0; i < 2; ++i) {
					gasmix[i].helium = data[offset + 30 + i];
				}
			} else if (type == LOG_RECORD_OPENING_1) {
				for (unsigned int i = 2; i < NFIXED; ++i) {
					gasmix[i].helium = data[offset + 1 + i - 2];
				}
			} else if (type == LOG_RECORD_OPENING_4) {
				// Log version
				logversion = data[offset + 16];

				// Air integration mode
				if (logversion >= 7) {
					aimode = data[offset + 28];
					if (logversion < 13) {
						if (aimode == 1 || aimode == 2) {
							tank[aimode - 1].enabled = 1;
						} else if (aimode == 3) {
							tank[0].enabled = 1;
							tank[1].enabled = 1;
						}
					}
					if (logversion < 14) {
						if (aimode == AI_HPCCR) {
							for (unsigned int i = 0; i < 2; ++i) {
								tank[4 + i].enabled = 1;
								tank[4 + i].usage = i == 0 ? DC_USAGE_DILUENT : DC_USAGE_OXYGEN;
							}
							hpccr = 1;
						}
					}
				}

				// Gas mix on/off state.
				unsigned int state = array_uint16_be (data + offset + 17);
				for (unsigned int i = 0; i < NFIXED; ++i) {
					gasmix[i].enabled = (state & (1 << i)) != 0;
				}

				unsigned int gtrmode = data[offset + 29];
				if (popcount(gtrmode) >= 2) {
					for (unsigned int i = 0; i < 4; ++i) {
						if (gtrmode & (1 << i)) {
							tank[i].usage = DC_USAGE_SIDEMOUNT;
						}
					}
				}
			} else if (type == LOG_RECORD_OPENING_5) {
				if (logversion >= 9) {
					tank[0].serial = array_convert_bcd2dec (data + offset + 1, 3);
					tank[0].pressure_max = array_uint16_be(data + offset + 6);
					tank[0].pressure_reserve = array_uint16_be(data + offset + 8);

					tank[1].serial = array_convert_bcd2dec(data + offset + 10, 3);
					tank[1].pressure_max = array_uint16_be(data + offset + 15);
					tank[1].pressure_reserve = array_uint16_be(data + offset + 17);
				}
			} else if (type == LOG_RECORD_OPENING_6) {
				if (logversion >= 13) {
					tank[0].enabled = data[offset + 19];
					memcpy (tank[0].name, data + offset + 20, sizeof (tank[0].name));

					tank[1].enabled = data[offset + 22];
					memcpy (tank[1].name, data + offset + 23, sizeof (tank[1].name));

					tank[2].serial = array_convert_bcd2dec(data + offset + 25, 3);
					tank[2].pressure_max = array_uint16_be(data + offset + 28);
					tank[2].pressure_reserve = array_uint16_be(data + offset + 30);
				}
			} else if (type == LOG_RECORD_OPENING_7) {
				if (logversion >= 13) {
					tank[2].enabled =  data[offset + 1];
					memcpy (tank[2].name, data + offset + 2, sizeof (tank[2].name));

					tank[3].serial = array_convert_bcd2dec(data + offset + 4, 3);
					tank[3].pressure_max = array_uint16_be(data + offset + 7);
					tank[3].pressure_reserve = array_uint16_be(data + offset + 9);
					tank[3].enabled = data[offset + 11];
					memcpy (tank[3].name, data + offset + 12, sizeof (tank[3].name));
				}
			}
		} else if (type >= LOG_RECORD_CLOSING_0 && type <= LOG_RECORD_CLOSING_7) {
			// Closing record
			parser->closing[type - LOG_RECORD_CLOSING_0] = offset;
		} else if (type == LOG_RECORD_FINAL) {
			// Final record
			parser->final = offset;
		}

		offset += parser->samplesize;
	}

	// Verify the required opening/closing records.
	// At least in firmware v71 and newer, Petrel and Petrel 2 also use PNF,
	// and there opening/closing record 5 (which contains AI information plus
	// the sample interval) don't appear to exist - so don't mark them as required
	for (unsigned int i = 0; i <= 4; ++i) {
		if (parser->opening[i] == UNDEFINED || parser->closing[i] == UNDEFINED) {
			ERROR (abstract->context, "Opening or closing record %u not found.", i);
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Cache sensor calibration for later use
	unsigned int nsensors = 0, ndefaults = 0;
	unsigned int base = parser->opening[3] + (pnf ? 6 : 86);
	for (size_t i = 0; i < 3; ++i) {
		unsigned int calibration = array_uint16_be(data + base + 1 + i * 2);
		parser->calibration[i] = calibration / 100000.0;
		if (parser->model == PREDATOR) {
			// The Predator expects the mV output of the cells to be
			// within 30mV to 70mV in 100% O2 at 1 atmosphere. If the
			// calibration value is scaled with a factor 2.2, then the
			// sensors lines up and matches the average.
			parser->calibration[i] *= 2.2;
		}
		if (data[base] & (1 << i)) {
			if (calibration == 2100) {
				ndefaults++;
			}
			nsensors++;
		}
	}
	if (nsensors && nsensors == ndefaults) {
		// If all (calibrated) sensors still have their factory default
		// calibration values (2100), they are probably not calibrated
		// properly. To avoid returning incorrect ppO2 values to the
		// application, they are manually disabled (e.g. marked as
		// uncalibrated).
		WARNING (abstract->context, "Disabled all O2 sensors due to a default calibration value.");
		parser->calibrated = 0;
	} else {
		parser->calibrated = data[base];
	}

	// Get the dive mode from the header (if available).
	if (logversion >= 8) {
		divemode = data[parser->opening[4] + (pnf ? 1 : 112)];
	}

	// Get the correct model number from the final block.
	if (parser->final != UNDEFINED) {
		parser->model = data[parser->final + 13];
		DEBUG (abstract->context, "Device: model=%u, serial=%u, firmware=%u",
			data[parser->final + 13],
			array_uint32_be (data + parser->final + 2),
			bcd2dec (data[parser->final + 10]));
	}

	// Fix the Teric tank serial number.
	if (parser->model == TERIC) {
		for (unsigned int i = 0; i < NTANKS; ++i) {
			tank[i].serial =
				((tank[i].serial / 10000) % 100) +
				((tank[i].serial /   100) % 100) * 100 +
				((tank[i].serial        ) % 100) * 10000;
		}
	}

	// Cache the data for later use.
	parser->pnf = pnf;
	parser->logversion = logversion;
	parser->headersize = headersize;
	parser->footersize = footersize;
	parser->ngasmixes = 0;
	if (divemode != M_FREEDIVE) {
		for (unsigned int i = 0; i < ngasmixes; ++i) {
			if (gasmix[i].oxygen == 0 && gasmix[i].helium == 0)
				continue;
			if (!gasmix[i].enabled && !gasmix[i].active)
				continue;
			if (gasmix[i].diluent && !shearwater_predator_is_ccr (divemode))
				continue;
			parser->gasmix[parser->ngasmixes] = gasmix[i];
			parser->ngasmixes++;
		}
	}
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		if (tank[i].active) {
			parser->tankidx[i] = parser->ntanks;
			parser->tank[parser->ntanks] = tank[i];
			parser->ntanks++;
		} else {
			parser->tankidx[i] = UNDEFINED;
		}
	}
	parser->aimode = aimode;
	parser->hpccr = hpccr;
	parser->divemode = divemode;
	parser->units = data[parser->opening[0] + 8];
	parser->atmospheric = array_uint16_be (data + parser->opening[1] + (parser->pnf ? 16 : 47));
	parser->density = array_uint16_be (data + parser->opening[3] + (parser->pnf ? 3 : 83));
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int decomodel_idx = parser->pnf ? parser->opening[2] + 18 : 67;
	unsigned int gf_idx        = parser->pnf ? parser->opening[0] +  4 : 4;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_decomodel_t *decomodel = (dc_decomodel_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->pnf)
				*((unsigned int *) value) = array_uint24_be (data + parser->closing[0] + 6);
			else
				*((unsigned int *) value) = array_uint16_be (data + parser->closing[0] + 6) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (parser->units == IMPERIAL)
				*((double *) value) = array_uint16_be (data + parser->closing[0] + 4) * FEET;
			else
				*((double *) value) = array_uint16_be (data + parser->closing[0] + 4);
			if (parser->pnf)
				*((double *)value) /= 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->usage = parser->gasmix[flags].diluent ? DC_USAGE_DILUENT : DC_USAGE_NONE;
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->beginpressure = parser->tank[flags].beginpressure * 2 * PSI / BAR;
			tank->endpressure   = parser->tank[flags].endpressure   * 2 * PSI / BAR;
			tank->gasmix = DC_GASMIX_UNKNOWN;
			if (shearwater_predator_is_ccr (parser->divemode) && !parser->hpccr) {
				switch (parser->tank[flags].name[0]) {
				case 'O':
					tank->usage = DC_USAGE_OXYGEN;
					break;
				case 'D':
					tank->usage = DC_USAGE_DILUENT;
					break;
				default:
					tank->usage = DC_USAGE_NONE;
					break;
				}
			} else {
				tank->usage = parser->tank[flags].usage;
			}
			break;
		case DC_FIELD_SALINITY:
			if (parser->density == 1000)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = parser->density;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = parser->atmospheric / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			switch (parser->divemode) {
			case M_CC:
			case M_CC2:
				*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
				break;
			case M_SC:
				*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
				break;
			case M_OC_TEC:
			case M_OC_REC:
				*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
				break;
			case M_GAUGE:
			case M_PPO2:
				*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
				break;
			case M_FREEDIVE:
				*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
				break;
			default:
				return DC_STATUS_DATAFORMAT;
			}
			break;
		case DC_FIELD_DECOMODEL:
			switch (data[decomodel_idx]) {
			case GF:
				decomodel->type = DC_DECOMODEL_BUHLMANN;
				decomodel->conservatism = 0;
				decomodel->params.gf.low  = data[gf_idx + 0];
				decomodel->params.gf.high = data[gf_idx + 1];
				break;
			case VPMB:
			case VPMB_GFS:
				decomodel->type = DC_DECOMODEL_VPM;
				decomodel->conservatism = data[decomodel_idx + 1];
				break;
			case DCIEM:
				decomodel->type = DC_DECOMODEL_DCIEM;
				decomodel->conservatism = 0;
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
shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Previous gas mix.
	unsigned int o2_previous = UNDEFINED, he_previous = UNDEFINED, dil_previous = UNDEFINED;

	// Sample interval.
	unsigned int time = 0;
	unsigned int interval = 10000;
	if (parser->pnf && parser->logversion >= 9 && parser->opening[5] != UNDEFINED) {
		interval = array_uint16_be (data + parser->opening[5] + 23);
	}

	unsigned int pnf = parser->pnf;
	unsigned int offset = parser->headersize;
	unsigned int length = size - parser->footersize;
	while (offset + parser->samplesize <= length) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Get the record type.
		unsigned int type = pnf ? data[offset] : LOG_RECORD_DIVE_SAMPLE;

		if (type == LOG_RECORD_DIVE_SAMPLE) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

			// Depth (1/10 m or ft).
			unsigned int depth = array_uint16_be (data + pnf + offset);
			if (parser->units == IMPERIAL)
				sample.depth = depth * FEET / 10.0;
			else
				sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

			// Temperature (°C or °F).
			int temperature = (signed char) data[offset + pnf + 13];
			if (temperature < 0) {
				// Fix negative temperatures.
				temperature += 102;
				if (temperature > 0) {
					temperature = 0;
				}
			}
			if (parser->units == IMPERIAL)
				sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
			else
				sample.temperature = temperature;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);

			// Status flags.
			unsigned int status = data[offset + pnf + 11];
			unsigned int ccr = (status & OC) == 0;

			if (ccr) {
				// PPO2
				if ((status & PPO2_EXTERNAL) == 0) {
					sample.ppo2.sensor = DC_SENSOR_NONE;
					sample.ppo2.value = data[offset + pnf + 6] / 100.0;
					if (callback) callback (DC_SAMPLE_PPO2, &sample, userdata);

					sample.ppo2.sensor = 0;
					sample.ppo2.value = data[offset + pnf + 12] * parser->calibration[0];
					if (callback && (parser->calibrated & 0x01)) callback (DC_SAMPLE_PPO2, &sample, userdata);

					sample.ppo2.sensor = 1;
					sample.ppo2.value = data[offset + pnf + 14] * parser->calibration[1];
					if (callback && (parser->calibrated & 0x02)) callback (DC_SAMPLE_PPO2, &sample, userdata);

					sample.ppo2.sensor = 2;
					sample.ppo2.value = data[offset + pnf + 15] * parser->calibration[2];
					if (callback && (parser->calibrated & 0x04)) callback (DC_SAMPLE_PPO2, &sample, userdata);
				}

				// Setpoint
				if (parser->petrel) {
					sample.setpoint = data[offset + pnf + 18] / 100.0;
				} else {
					// this will only ever be called for the actual Predator, so no adjustment needed for PNF
					if (status & SETPOINT_HIGH) {
						sample.setpoint = data[18] / 100.0;
					} else {
						sample.setpoint = data[17] / 100.0;
					}
				}
				if (callback) callback (DC_SAMPLE_SETPOINT, &sample, userdata);
			}

			// CNS
			if (parser->petrel) {
				sample.cns = data[offset + pnf + 22] / 100.0;
				if (callback) callback (DC_SAMPLE_CNS, &sample, userdata);
			}

			// Gaschange.
			unsigned int o2 = data[offset + pnf + 7];
			unsigned int he = data[offset + pnf + 8];
			if ((o2 != o2_previous || he != he_previous || ccr != dil_previous) &&
				(o2 != 0 || he != 0)) {
				unsigned int idx = shearwater_predator_find_gasmix (parser, o2, he, ccr);
				if (idx >= parser->ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix.");
					return DC_STATUS_DATAFORMAT;
				}

				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, &sample, userdata);
				o2_previous = o2;
				he_previous = he;
				dil_previous = ccr;
			}

			// Deco stop / NDL.
			unsigned int decostop = array_uint16_be (data + offset + pnf + 2);
			if (decostop) {
				sample.deco.type = DC_DECO_DECOSTOP;
				if (parser->units == IMPERIAL)
					sample.deco.depth = decostop * FEET;
				else
					sample.deco.depth = decostop;
			} else {
				sample.deco.type = DC_DECO_NDL;
				sample.deco.depth = 0.0;
			}
			sample.deco.time = data[offset + pnf + 9] * 60;
			sample.deco.tts = array_uint16_be (data + offset + pnf + 4) * 60;
			if (callback) callback (DC_SAMPLE_DECO, &sample, userdata);

			// for logversion 7 and newer (introduced for Perdix AI)
			// detect tank pressure
			if (parser->logversion >= 7) {
				const unsigned int idx[2] = {27, 19};
				for (unsigned int i = 0; i < 2; ++i) {
					// Tank pressure
					// Values above 0xFFF0 are special codes:
					//    0xFFFF AI is off
					//    0xFFFE No comms for 90 seconds+
					//    0xFFFD No comms for 30 seconds
					//    0xFFFC Transmitter not paired
					// For regular values, the top 4 bits contain the battery
					// level (0=normal, 1=critical, 2=warning), and the lower 12
					// bits the tank pressure in units of 2 psi.
					unsigned int pressure = array_uint16_be (data + offset + pnf + idx[i]);
					unsigned int id = (parser->aimode == AI_HPCCR ? 4 : 0) + i;
					if (pressure < 0xFFF0) {
						pressure &= 0x0FFF;
						if (pressure) {
							sample.pressure.tank = parser->tankidx[id];
							sample.pressure.value = pressure * 2 * PSI / BAR;
							if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
						}
					}
				}

				// Gas time remaining in minutes
				// Values above 0xF0 are special codes:
				//    0xFF Not paired
				//    0xFE No communication
				//    0xFD Not available in current mode
				//    0xFC Not available because of DECO
				//    0xFB Tank size or max pressure haven’t been set up
				if (data[offset + pnf + 21] < 0xF0) {
					sample.rbt = data[offset + pnf + 21];
					if (callback) callback (DC_SAMPLE_RBT, &sample, userdata);
				}
			}
		} else if (type == LOG_RECORD_DIVE_SAMPLE_EXT) {
			// Tank pressure
			if (parser->logversion >= 13) {
				for (unsigned int i = 0; i < 2; ++i) {
					unsigned int pressure = array_uint16_be (data + offset + pnf + i * 2);
					unsigned int id = 2 + i;
					if (pressure < 0xFFF0) {
						pressure &= 0x0FFF;
						if (pressure) {
							sample.pressure.tank = parser->tankidx[id];
							sample.pressure.value = pressure * 2 * PSI / BAR;
							if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
						}
					}
				}
			}
			// Tank pressure (HP CCR)
			if (parser->logversion >= 14) {
				for (unsigned int i = 0; i < 2; ++i) {
					unsigned int pressure = array_uint16_be (data + offset + pnf + 4 + i * 2);
					unsigned int id = 4 + i;
					if (pressure) {
						sample.pressure.tank = parser->tankidx[id];
						sample.pressure.value = pressure * 2 * PSI / BAR;
						if (callback) callback (DC_SAMPLE_PRESSURE, &sample, userdata);
					}
				}
			}
		} else if (type == LOG_RECORD_FREEDIVE_SAMPLE) {
			// A freedive record is actually 4 samples, each 8-bytes,
			// packed into a standard 32-byte sized record. At the end
			// of a dive, unused partial records will be 0 padded.
			for (unsigned int i = 0; i < 4; ++i) {
				unsigned int idx = offset + i * SZ_SAMPLE_FREEDIVE;

				// Ignore empty samples.
				if (array_isequal (data + idx, SZ_SAMPLE_FREEDIVE, 0x00)) {
					break;
				}

				// Time (seconds).
				time += interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, &sample, userdata);

				// Depth (absolute pressure in millibar)
				unsigned int depth = array_uint16_be (data + idx + 1);
				sample.depth = (signed int)(depth - parser->atmospheric) * (BAR / 1000.0) / (parser->density * GRAVITY);
				if (callback) callback (DC_SAMPLE_DEPTH, &sample, userdata);

				// Temperature (1/10 °C).
				int temperature = (signed short) array_uint16_be (data + idx + 3);
				sample.temperature = temperature / 10.0;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}
		} else if (type == LOG_RECORD_INFO_EVENT) {
			unsigned int event = data[offset + 1];
			unsigned int DC_ATTR_UNUSED timestamp = array_uint32_be (data + offset + 4);
			unsigned int w1 = array_uint32_be (data + offset + 8);
			unsigned int w2 = array_uint32_be (data + offset + 12);

			if (event == INFO_EVENT_TAG_LOG) {
				// Compass heading
				if (w1 != 0xFFFFFFFF) {
					sample.bearing = w1;
					if (callback) callback (DC_SAMPLE_BEARING, &sample, userdata);
				}

				// Tag
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = w2;
				if (callback) callback (DC_SAMPLE_EVENT, &sample, userdata);
			}
		}

		offset += parser->samplesize;
	}

	return DC_STATUS_SUCCESS;
}
