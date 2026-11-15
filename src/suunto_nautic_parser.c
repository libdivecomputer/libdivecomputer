/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Jef Driesen
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

/*
 * Parses the dives stored by suunto_nautic_device_foreach(): the /Summary
 * and the decompressed /Data, both SBEM containers (see suunto_nautic.h).
 * A container is an 8-byte header (SBEM0103) and a series of chunks:
 *
 *   ID LENGTH DATA(LENGTH)
 *
 * The id is one byte, or 0xFF and the id as a uint16 LE; the length is one
 * byte, or 0xFF and the length as a uint32 LE.
 *
 * Decoded chunks: 0x12 (1Hz absolute pressure / temperature), 0x16
 * (depth, cylinder pressures, gas time remaining, NDL, time-to-surface),
 * 0x17 (surface pressure), 0x0B (GPS), 0x0F (heart rate, Ocean only),
 * 0x08 (activity -> dive mode), plus the dynamically-assigned dive-event
 * subgroups. Chunks 0x0E (satellite info) and 0x14 (battery) map to no
 * dc_field/dc_sample, and are emitted through DC_SAMPLE_VENDOR. Chunks
 * 0x23/0x24 are raw accelerometer / gyroscope dumps for client-side dead
 * reckoning, emitted through DC_SAMPLE_VENDOR. Unknown chunk ids are
 * skipped, so extending the decoder is additive.
 *
 * Sample time is delta-encoded: every chunk except the timeline base
 * (0x01) begins with a signed int16 LE millisecond delta. The dive start
 * time (dc_parser_get_datetime()) is reconstructed from the only absolute
 * clock in the stream: each GPS fix (chunk 0x0B) carries an absolute UTC
 * in milliseconds, so start = gps_utc - gps_relative_time. A dive with no
 * surface GPS fix has no absolute clock, so datetime is unsupported for it.
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "suunto_nautic.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define CHUNK_TIMELINE_BASE   0x01
#define CHUNK_ACTIVITY        0x08 // [timeDelta:2][sportId:1][customModeId: ascii]
// Suunto sport ids seen on the Nautic/Ocean, from the app's ActivityType:
// 51 = scuba, 61 = free diving, 62 = mermaiding. Only the last two are apnea.
#define SPORT_ID_FREEDIVE     61
#define SPORT_ID_MERMAIDING   62
#define CHUNK_GPS             0x0B
#define CHUNK_GPS_ACCURACY    0x0E // [timeDelta:2][dEHPE:int8][dEVPE:int8][?:2]
#define CHUNK_HEARTRATE       0x0F // [timeDelta:2][hr:uint8 bpm] -- Ocean wrist HR only
#define CHUNK_BATTERY         0x14 // [timeDelta:2][current:int16][voltage:uint16 mV][charge:uint8 %]
// High-rate IMU: [timeDelta:2][algoTS:uint32][accel/gyro/mag X,Y,Z:int16], a
// 24-byte payload. The chunk id is FIRMWARE-DEPENDENT: 0x23 on the 195-byte
// extended-status watches (Nautic), 0x22 on the 141-byte ones (Nautic S /
// Ocean). Both are matched by (id in {0x22,0x23} AND payload >= 24); see the
// IMU handler. Keying on the id alone silently dropped IMU on 141-byte watches.
#define CHUNK_IMU             0x23
#define CHUNK_IMU_ALT         0x22
// DiveRouteFeatures: 5x uint16 (16-byte payload), id 0x24 on 195-byte watches
// and 0x23 (small) on 141-byte ones. These are input features to the app's
// dive-route tracking algorithm, not the resulting X/Y/Z track (which the
// device doesn't store; the app dead-reckons it from the raw IMU above). The
// meaning of the individual features is unknown; they are carried through.
#define CHUNK_DIVEROUTE_FEATURES 0x24

// libdivecomputer has no dedicated battery/GPS-accuracy/IMU sample types, so
// these are delivered through the generic DC_SAMPLE_VENDOR channel tagged
// SAMPLE_VENDOR_SUUNTO_NAUTIC. Byte 0 of every vendor record is one of these
// kinds; the rest is a canonical little-endian payload (see each handler).
#define VENDOR_KIND_BATTERY      1 // [voltage_mv:u16][charge_permille:u16]
#define VENDOR_KIND_GPS_ACCURACY 2 // [ehpe_m:u16][evpe_m:u16]
#define VENDOR_KIND_IMU          3 // [ax,ay,az,gx,gy,gz,mx,my,mz:int16]
#define VENDOR_KIND_DIVEROUTE_FEATURES 4 // [f0,f1,f2,f3,f4:uint16] DiveRouteFeatures (algo inputs; not the X/Y/Z track)
#define VENDOR_KIND_GF           5 // [gf99:int16][gf_surface:int16][gf_leading:int16] (%)
#define CHUNK_PROFILE_1HZ     0x12
#define CHUNK_EXTENDED_STATUS 0x16 // VARIABLE length (195 on Ocean/Nautic, 141
                                   // on Nautic S and other firmware); fields are
                                   // read by offset, each guarded by chunk.size.
                                   // Deliberately not in the fixed-length table.
#define CHUNK_SURFACE_PRESSURE 0x17
// Dive-event groups: the chunk id IS the event subgroup, each record is
// [timeDelta:2 LE][Type:1][Active:1] (Active 1=begin/onset, 0=end/cleared).
// Type indexes the subgroup's own enum (see the descriptor schema).
#define CHUNK_EVENT_ALARM     0x18
#define CHUNK_EVENT_WARNING   0x19
#define CHUNK_EVENT_NOTIFY    0x1A
#define CHUNK_EVENT_STATE     0x1B
#define CHUNK_DIVE_STATE      0x1C // [timeDelta:2][state:1]; 0=Idling,1=Diving,2=Recovering
#define CHUNK_DIVE_STATUS     0x1E // [timeDelta:2][active:1]; the DiveActive flag
#define CHUNK_OOAM            0x1D // [timeDelta:2][type:1]; one-shot dive-end reason (Ooam.Type)
#define CHUNK_GAS_SWITCH      0x1F // [timeDelta:2][gasnumber:int16 LE]

// DiveState values (CHUNK_DIVE_STATE payload).
#define DIVE_STATE_IDLING     0
#define DIVE_STATE_DIVING     1
#define DIVE_STATE_RECOVERING 2

#define MAX_TANKS 8
#define MAX_GASMIXES 4

// The /Summary SBEM section starts with the dive header, with the settings
// the dive was made with, which the profile (/Data) doesn't carry. The
// driver stores the /Summary in front of the profile so this parser can
// expose them the standard way. Like the ids from 0x22 up in the profile,
// the id of the header moves with the firmware: 0x20 on the Nautic S and
// Ocean, 0x21 on the Nautic, so it is recognized as the first chunk. Its
// layout is the same on both, as far as decoded:
#define SUMMARY_HEADER     0x20
#define SUMMARY_HEADER_ALT 0x21
#define HEADER_GF_HIGH     37 // uint16 LE, %
#define HEADER_GF_LOW      39 // uint16 LE, %
#define HEADER_WATER_TYPE  48 // uint8 enum: 0 fresh, 1 EN13319, 2 salt
// GF low is at +39 and GF high at +37. This ordering yields valid low <= high
// on two devices (Nautic 35/75, Nautic S 40/85); the reverse gives low > high.
// Gas/cylinder records: an array starting at HEADER_GAS, one 45-byte
// record per configured gas. The record index equals the cylinder slot
// (GasNumber), so tank i uses gas i. Within a record: O2% at +1, He% at +3,
// the configured PO2 max (float32 LE, bar) at +5, and cylinder water capacity
// (float32 LE, m^3) at +9. Confirmed on real single- and dual-gas dives; a
// lone-gas dive stops at the first implausible slot.
#define HEADER_GAS        185
#define HEADER_GAS_SIZE   45
#define GAS_O2            1  // uint16 LE, %
#define GAS_HE            3  // uint16 LE, %
#define GAS_PO2MAX        5  // float32 LE, bar
#define GAS_VOLUME        9  // float32 LE, m^3

typedef struct suunto_nautic_tank_t {
	unsigned int used;
	double beginpressure; // bar
	double endpressure;   // bar
	unsigned int gasmix;  // gas index (== cylinder slot / GasNumber)
} suunto_nautic_tank_t;

typedef struct suunto_nautic_parser_t {
	dc_parser_t base;
	unsigned int cached;
	unsigned int divetime; // seconds
	double maxdepth;       // meters
	double avgdepth;       // meters
	unsigned int have_temperature;
	double temperature_surface; // first (surface) reading
	double temperature_minimum;
	double temperature_maximum;
	unsigned int ntanks;
	suunto_nautic_tank_t tank[MAX_TANKS];
	unsigned int have_location;
	dc_location_t location;
	unsigned int have_atmospheric;
	double atmospheric; // bar
	unsigned int have_datetime;
	dc_ticks_t datetime; // dive start, UNIX seconds
	dc_divemode_t divemode; // from the CHUNK_ACTIVITY sport id; OC unless apnea
	// From the /Summary SBEM section in front of the profile.
	unsigned int ngasmixes;
	dc_gasmix_t gasmix[MAX_GASMIXES];
	double gasvolume[MAX_GASMIXES]; // cylinder water capacity, litres (0 = unknown)
	unsigned int have_decomodel;
	dc_decomodel_t decomodel;
	unsigned int have_ppo2max;
	double ppo2max; // bar, configured oxygen partial-pressure limit
	unsigned int have_salinity;
	dc_salinity_t salinity;
} suunto_nautic_parser_t;

typedef dc_status_t (*suunto_nautic_sbem_callback_t) (dc_context_t *context, unsigned int id, const unsigned char data[], unsigned int size, void *userdata);

// The state of the walk over the profile, besides the fields it fills in.
typedef struct suunto_nautic_profile_t {
	suunto_nautic_parser_t *parser;
	dc_sample_callback_t callback;
	void *userdata;
	// Time is delta-encoded: every chunk (except the timeline base 0x01)
	// begins with a signed int16 LE millisecond delta at payload[0:2]. The
	// running sum is the absolute sample time -- there is no per-sample
	// absolute timestamp and no fixed sample rate.
	//
	// time_ms is the raw running sum; deltas are always added to it. sample_ms
	// is the monotonic (never-decreasing) view of that sum, and is what every
	// emitted sample time and the dive-start datetime are derived from. They
	// differ only across a GPS / GPS-accuracy chunk: those carry a delta on
	// their own back-dated sub-chain that rewinds the sum a few hundred ms,
	// after which the next chunk's delta (measured from the rewound point)
	// walks it forward again. The raw sum stays correct for span arithmetic,
	// but a sample emitted at the rewound value goes backwards in the profile
	// and blows up the downstream depth interpolation (submersion-libdc#1).
	int time_ms;
	int sample_ms;
	// Dive phase, from CHUNK_DIVE_STATE. Dive time is the TOTAL time spent in
	// the Diving state (sum of every Diving span), which matches the app's
	// DiveTimeMax on multi-level dives that briefly surface mid-dive -- taking
	// only the longest single span undercounts those. A spurious ~0 s startup
	// blip contributes nothing. Average depth is taken over Diving samples only;
	// counting from the first raw sample would include the pre-dive/surface
	// phase and skew both low.
	unsigned int dive_state;
	int diving_start_ms;
	int total_dive_ms;
	double depth_sum;
	unsigned int depth_count;
	// Each cylinder slot has two pressure fields: Pressure (+2, the main
	// transmitter) and Pressure2 (+6, a sidemount second transmitter). Map
	// each (slot, field) with data to a compacted tank index. Pressure2 is
	// gated: a real second transmitter produces a continuous non-zero
	// curve, whereas a slot with no second transmitter emits a single
	// spurious Pressure2 sample then zeros -- so only accept Pressure2 once
	// it has read non-zero at least twice (p2_first holds the first value).
	int tankmap[MAX_TANKS * 2];
	int p2_first[MAX_TANKS];
	// GPS horizontal/vertical position error, int8-delta-accumulated (chunk 0x0E).
	int ehpe, evpe;
} suunto_nautic_profile_t;

typedef struct suunto_nautic_summary_t {
	suunto_nautic_parser_t *parser;
	unsigned int nchunks;
} suunto_nautic_summary_t;

static dc_status_t suunto_nautic_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t suunto_nautic_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t suunto_nautic_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t suunto_nautic_parser_vtable = {
	sizeof(suunto_nautic_parser_t),
	DC_FAMILY_SUUNTO_NAUTIC,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	suunto_nautic_parser_get_datetime, /* datetime */
	suunto_nautic_parser_get_field,
	suunto_nautic_parser_samples_foreach,
	NULL, /* destroy */
};

dc_status_t
suunto_nautic_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	suunto_nautic_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	parser = (suunto_nautic_parser_t *) dc_parser_allocate (context, &suunto_nautic_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	parser->cached = 0;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}

// Walk the chunks of an SBEM container, and pass each one to the callback.
// The container starts with an 8-byte header (SBEM0103), and every chunk is
// ID LENGTH DATA(LENGTH). The id is one byte, or 0xFF and the id as a uint16;
// the length is one byte, or 0xFF and the length as a uint32.
static dc_status_t
suunto_nautic_sbem_parse (dc_context_t *context, const unsigned char data[], unsigned int size, suunto_nautic_sbem_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned int offset = 0;

	if (offset + 8 > size) {
		ERROR (context, "Buffer overflow detected (%08x).", offset);
		return DC_STATUS_DATAFORMAT;
	}

	if (memcmp (data, "SBEM", 4) != 0) {
		ERROR (context, "Unexpected SBEM header.");
		return DC_STATUS_DATAFORMAT;
	}

	offset += 8;

	while (offset < size) {
		unsigned int header = 0;

		// Read the chunk id.
		if (offset + header + 1 > size) {
			ERROR (context, "Buffer overflow detected (%08x).", offset);
			return DC_STATUS_DATAFORMAT;
		}
		unsigned int id = data[offset + header];
		header++;
		if (id == 0xFF) {
			if (offset + header + 2 > size) {
				ERROR (context, "Buffer overflow detected (%08x).", offset);
				return DC_STATUS_DATAFORMAT;
			}
			id = array_uint16_le (data + offset + header);
			header += 2;
		}

		// Read the chunk length.
		if (offset + header + 1 > size) {
			ERROR (context, "Buffer overflow detected (%08x).", offset);
			return DC_STATUS_DATAFORMAT;
		}
		unsigned int length = data[offset + header];
		header++;
		if (length == 0xFF) {
			if (offset + header + 4 > size) {
				ERROR (context, "Buffer overflow detected (%08x).", offset);
				return DC_STATUS_DATAFORMAT;
			}
			length = array_uint32_le (data + offset + header);
			header += 4;
		}

		// Check the chunk length. The checks above guarantee offset + header
		// <= size, and comparing the remainder can't overflow, unlike
		// offset + header + length with a 32-bit length.
		if (length > size - offset - header) {
			ERROR (context, "Buffer overflow detected (%08x).", offset);
			return DC_STATUS_DATAFORMAT;
		}

		status = callback (context, id, data + offset + header, length, userdata);
		if (status != DC_STATUS_SUCCESS)
			return status;

		offset += header + length;
	}

	return status;
}

// Decode gradient factors, water type, gas mixes and per-gas cylinder size
// from the dive header, the first chunk of the /Summary; the other chunks
// aren't decoded. Gases are validated by plausibility (O2 in 1..100, He in
// 0..100-O2) and counted until the first implausible slot, since unused
// slots hold unrelated bytes.
static dc_status_t
suunto_nautic_parser_summary (dc_context_t *context, unsigned int id, const unsigned char data[], unsigned int size, void *userdata)
{
	suunto_nautic_summary_t *s = (suunto_nautic_summary_t *) userdata;
	suunto_nautic_parser_t *parser = s->parser;

	if (s->nchunks++ != 0)
		return DC_STATUS_SUCCESS;

	if (id != SUMMARY_HEADER && id != SUMMARY_HEADER_ALT) {
		ERROR (context, "Unexpected dive header id (%u).", id);
		return DC_STATUS_DATAFORMAT;
	}

	if (size < HEADER_GAS + MAX_GASMIXES * HEADER_GAS_SIZE) {
		ERROR (context, "Unexpected dive header size (%u).", size);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int low = array_uint16_le (data + HEADER_GF_LOW);
	unsigned int high = array_uint16_le (data + HEADER_GF_HIGH);
	parser->decomodel.type = DC_DECOMODEL_BUHLMANN;
	parser->decomodel.conservatism = 0;
	parser->decomodel.params.gf.low = low;
	parser->decomodel.params.gf.high = high;
	parser->have_decomodel = 1;

	unsigned int wt = data[HEADER_WATER_TYPE];
	if (wt <= 2) {
		static const double density[] = {1000.0, 1020.0, 1030.0};
		parser->salinity.type = wt ? DC_WATER_SALT : DC_WATER_FRESH;
		parser->salinity.density = density[wt];
		parser->have_salinity = 1;
	}

	for (unsigned int i = 0; i < MAX_GASMIXES; i++) {
		const unsigned char *gas = data + HEADER_GAS + i * HEADER_GAS_SIZE;
		unsigned int o2 = array_uint16_le (gas + GAS_O2);
		unsigned int he = array_uint16_le (gas + GAS_HE);
		if (o2 < 1 || o2 > 100 || he > 100 || o2 + he > 100)
			break; // unused/implausible slot -- stop
		unsigned int g = parser->ngasmixes;
		parser->gasmix[g].oxygen = o2 / 100.0;
		parser->gasmix[g].helium = he / 100.0;
		parser->gasmix[g].nitrogen = 1.0 - (o2 + he) / 100.0;
		parser->gasmix[g].usage = DC_USAGE_NONE;
		// Per-gas cylinder water capacity (float32 m^3) -> litres, when it is
		// physically sane.
		double vol_m3 = array_float_le (gas + GAS_VOLUME);
		if (vol_m3 > 0.0 && vol_m3 < 1.0)
			parser->gasvolume[g] = vol_m3 * 1000.0;
		// The configured PO2 max (float32 bar) is a single watch setting held
		// in the first gas record. Record it once, when physically sane.
		if (g == 0) {
			double ppo2 = array_float_le (gas + GAS_PO2MAX);
			if (ppo2 >= 0.5 && ppo2 <= 3.0) {
				parser->ppo2max = ppo2;
				parser->have_ppo2max = 1;
			}
		}
		parser->ngasmixes++;
	}

	return DC_STATUS_SUCCESS;
}

// Map a Suunto dive-event to the closest dc_sample_event_t. The sub-group
// is the chunk id (0x18 Alarm / 0x19 Warning / 0x1A Notify / 0x1B State /
// 0x1D Ooam) and Type indexes that sub-group's enum -- the full tables are
// in the watch's descriptor library (see the Suunto Nautic protocol notes,
// sec. 9.4), and every documented Type is handled here rather than only
// the ones seen in a capture, so this holds for dives we have no export
// for. Suunto's vocabulary is far wider than libdivecomputer's, so many
// map to SAMPLE_EVENT_NONE (not emitted); the deco/gas state they mirror
// is already on DC_SAMPLE_DECO / DC_SAMPLE_GASMIX, and event.value carries
// the raw (sub-group<<8 | type) for a consumer that wants the exact label.
//
// The sub-group decides the fallback for an undocumented future Type:
//   - Alarm  -> SAMPLE_EVENT_VIOLATION. Suunto "Alarms" are the hard
//     red-alert class (deco/ceiling broken, PO2, depth, ...); a new one we
//     don't recognise is still serious, so surface it rather than hide it.
//   - Warning / Notify / State -> SAMPLE_EVENT_NONE. These are advisories
//     and status transitions; fabricating an alarm for an unknown one is
//     worse than dropping it (this is what made an ordinary no-deco dive
//     show a "deco violation": Warning "NoDecoTime" and State "Ndl
//     exceeded" fell through to VIOLATION / CEILING).
static unsigned int
suunto_nautic_map_event (unsigned int chunk_id, unsigned int type)
{
	switch (chunk_id) {
	case CHUNK_EVENT_ALARM: // red alerts
		switch (type) {
		case 1: case 2: return SAMPLE_EVENT_PO2;                 // PO2 Low / High
		case 3:         return SAMPLE_EVENT_AIRTIME;             // Tank Pressure
		case 4:         return SAMPLE_EVENT_AIRTIME;             // Gas Time
		case 5:         return SAMPLE_EVENT_ASCENT;              // Ascent Speed
		case 7: case 8: return SAMPLE_EVENT_OLF;                 // CNS 100% / OTU 300
		case 10:        return SAMPLE_EVENT_CEILING;             // Deco Stop Broken
		case 12:        return SAMPLE_EVENT_DEEPSTOP;            // Deep Stop Broken
		case 13:        return SAMPLE_EVENT_SAFETYSTOP_MANDATORY;// Safety Stop Broken
		case 31:        return SAMPLE_EVENT_MAXDEPTH;            // Depth
		case 50:        return SAMPLE_EVENT_NONE;                // Battery
		default:        return SAMPLE_EVENT_VIOLATION;
		}
	case CHUNK_EVENT_WARNING: // yellow advisories
		switch (type) {
		case 6:          return SAMPLE_EVENT_PO2;                // User PO2 High
		case 14: case 15:return SAMPLE_EVENT_OLF;               // CNS 80% / OTU 250
		case 28: case 29:return SAMPLE_EVENT_AIRTIME;           // User Tank Pressure / Gas Time
		case 32:         return SAMPLE_EVENT_DIVETIME;          // Dive Time
		default:         return SAMPLE_EVENT_NONE;              // 20 NoDecoTime, 30 Sidemount, 31 Depth,
		                                                        // 42 User Ndl, 44 Recovery, 50 Battery
		}
	case CHUNK_EVENT_STATE: // deco / stop transitions
		switch (type) {
		case 35:        return SAMPLE_EVENT_DECOSTOP;            // At Deco Stop (reached)
		case 36:        return SAMPLE_EVENT_DEEPSTOP;            // At Deep Stop (reached)
		case 37:        return SAMPLE_EVENT_SAFETYSTOP;          // At Safety Stop (reached)
		default:        return SAMPLE_EVENT_NONE;                // 19 Ndl exceeded, 38/39/40 "... Ahead"
		}
	case CHUNK_EVENT_NOTIFY: // status notifications
		switch (type) {
		case 11:        return SAMPLE_EVENT_GASCHANGE;           // Gas Switch
		case 28: case 29:return SAMPLE_EVENT_AIRTIME;           // User Tank Pressure / Gas Time
		case 32:        return SAMPLE_EVENT_DIVETIME;            // Dive Time
		default:        return SAMPLE_EVENT_NONE;                // 21 Setpoint, 41 Stop done, 42 User Ndl,
		                                                        // 60/61 Bearing, 62/63 Stopwatch, ...
		}
	case CHUNK_OOAM: // dive-end reason
		switch (type) {
		case 2:         return SAMPLE_EVENT_CEILING;             // Ceiling broken
		case 4:         return SAMPLE_EVENT_MAXDEPTH;            // Max depth
		default:        return SAMPLE_EVENT_NONE;                // 1 Battery, 3 SW crash, 5 Algo, 6 Gauge
		}
	default:
		return SAMPLE_EVENT_NONE;
	}
}

// Emit a Suunto-Nautic vendor record at the given sample time. Every such
// record starts with a VENDOR_KIND_* byte so a single vendor type can carry
// several kinds of non-standard telemetry (battery, GPS accuracy, IMU, ...).
static void
suunto_nautic_emit_vendor (dc_sample_callback_t callback, void *userdata,
	int time_ms, const unsigned char *rec, unsigned int size)
{
	dc_sample_value_t sample = {0};
	sample.time = (unsigned int) time_ms;
	callback (DC_SAMPLE_TIME, &sample, userdata);
	sample.vendor.type = SAMPLE_VENDOR_SUUNTO_NAUTIC;
	sample.vendor.size = size;
	sample.vendor.data = rec;
	callback (DC_SAMPLE_VENDOR, &sample, userdata);
}

static dc_status_t
suunto_nautic_parser_profile (dc_context_t *context, unsigned int id, const unsigned char data[], unsigned int size, void *userdata)
{
	suunto_nautic_profile_t *s = (suunto_nautic_profile_t *) userdata;
	suunto_nautic_parser_t *parser = s->parser;

	// Advance the clock by this chunk's leading ms delta (all groups
	// except the timeline base carry one).
	if (id != CHUNK_TIMELINE_BASE && size >= 2)
		s->time_ms += (int16_t) array_uint16_le (data);
	if (s->time_ms > s->sample_ms)
		s->sample_ms = s->time_ms;

	if (id == CHUNK_PROFILE_1HZ && size >= 18) {
		double temperature = array_uint16_le (data + 16) / 100.0 - 273.15;

		if (!parser->have_temperature) {
			// The first reading is taken at/near the surface at dive start.
			parser->temperature_surface = temperature;
			parser->temperature_minimum = parser->temperature_maximum = temperature;
			parser->have_temperature = 1;
		} else {
			if (temperature < parser->temperature_minimum)
				parser->temperature_minimum = temperature;
			if (temperature > parser->temperature_maximum)
				parser->temperature_maximum = temperature;
		}

		if (s->callback) {
			dc_sample_value_t sample = {0};
			sample.time = (unsigned int) s->sample_ms;
			s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
			sample.temperature = temperature;
			s->callback (DC_SAMPLE_TEMPERATURE, &sample, s->userdata);
		}
	} else if (id == CHUNK_EXTENDED_STATUS) {
		if (size >= 6) {
			double depth = array_float_le (data + 2);

			if (depth > parser->maxdepth)
				parser->maxdepth = depth;
			// Average only over the Diving phase (matches the app).
			if (s->dive_state == DIVE_STATE_DIVING) {
				s->depth_sum += depth;
				s->depth_count++;
			}

			if (s->callback) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) s->sample_ms;
				s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
				sample.depth = depth;
				s->callback (DC_SAMPLE_DEPTH, &sample, s->userdata);
			}
		}

		// Cylinders array: up to 8 elements of 18 bytes, starting at
		// offset 42 (idx:1, ?:1, pressure:4 LE Pa, pressure2:4 LE Pa, ...).
		// Read each tank only if its record fits: the shorter Nautic S
		// extended-status (141 B) holds fewer tank slots than the 195 B
		// Ocean/Nautic one, but tank 0's pressure is at the same offset 44
		// on both. Requiring the full 8-slot array (186 B) would drop all
		// tank pressure on the shorter Nautic S chunk.
		{
			// Read every cylinder slot whose FULL 18-byte record fits in
			// this chunk: the 195 B chunk holds all 8, the shorter 141 B
			// chunk (Nautic S and some Ocean firmware) holds slots 0-4.
			// Requiring the whole record (base + 18) rather than just the
			// pressure field excludes the partial slot past the end, whose
			// bytes would otherwise occasionally look like a phantom tank.
			// Each real record starts with its own index byte, so stop at
			// the first slot whose leading byte isn't its index. Slots 0-4
			// carry idx 0-4 on the 141 B chunk; tank 0 pressure is at offset
			// 44 on both layouts.
			for (unsigned int i = 0; i < MAX_TANKS; i++) {
				unsigned int base = 42 + i * 18;
				if (base + 18 > size)
					break; // full tank record doesn't fit this chunk
				if (data[base] != i)
					break; // not a real tank slot

				// Gas time remaining: uint32 LE seconds at record +10, for
				// the primary cylinder. 0xFFFFFFFF means not computed (no
				// AI, or not enough data yet). This is the app's
				// Cylinders[].GasTime; exposed as RBT minutes, the same way
				// suunto_eonsteel does.
				if (i == 0 && s->callback) {
					unsigned int gastime = array_uint32_le (data + base + 10);
					if (gastime != 0xFFFFFFFF && gastime != 0) {
						dc_sample_value_t sample = {0};
						sample.time = (unsigned int) s->sample_ms;
						s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
						sample.rbt = gastime / 60;
						s->callback (DC_SAMPLE_RBT, &sample, s->userdata);
					}
				}

				for (unsigned int field = 0; field < 2; field++) {
					unsigned int pressure_pa = array_uint32_le (data + base + 2 + field * 4);
					if (pressure_pa == 0)
						continue;
					unsigned int key = i * 2 + field;
					if (field == 1 && s->tankmap[key] < 0) {
						// Defer creating a Pressure2 tank until its second non-zero
						// reading, so a lone spurious sample doesn't become a phantom.
						if (s->p2_first[i] < 0) {
							s->p2_first[i] = (int) pressure_pa;
							continue;
						}
					}
					double bar = pressure_pa / 100000.0;
					if (s->tankmap[key] < 0) {
						if (parser->ntanks >= MAX_TANKS)
							continue;
						s->tankmap[key] = (int) parser->ntanks;
						parser->tank[parser->ntanks].used = 1;
						// Cylinder slot index == GasNumber, so this tank
						// breathes gas i (linked to gasmix[i] below).
						parser->tank[parser->ntanks].gasmix = i;
						// Seed begin from the first observed reading (the deferred
						// one for Pressure2); refined to the max below.
						parser->tank[parser->ntanks].beginpressure = (field == 1 && s->p2_first[i] >= 0)
							? s->p2_first[i] / 100000.0 : bar;
						parser->ntanks++;
					}
					unsigned int t = (unsigned int) s->tankmap[key];
					parser->tank[t].endpressure = bar;
					// Begin pressure = the highest reading seen: a cylinder only
					// loses pressure during a dive, so the max is the true start.
					// This ignores an initial dropout where a transmitter that
					// hasn't paired yet reports a spurious low value (e.g. ~9 bar
					// for the first minutes, then jumps to the real ~200 bar).
					if (bar > parser->tank[t].beginpressure)
						parser->tank[t].beginpressure = bar;
					if (s->callback) {
						dc_sample_value_t sample = {0};
						sample.time = (unsigned int) s->sample_ms;
						s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
						sample.pressure.tank = t;
						sample.pressure.value = bar;
						s->callback (DC_SAMPLE_PRESSURE, &sample, s->userdata);
					}
				}
			}
		}

		// Deco/safety fields. Offsets validated 244/244 against the app's
		// export: TTS uint16 @22 (s), NDL int16 @30 (s), Ceiling float32 @38
		// (m). Ceiling/NDL/TTS use the standard DC_SAMPLE_DECO channel.
		if (s->callback && size >= 42) {
			unsigned int tts = array_uint16_le (data + 22);
			int ndl = (int16_t) array_uint16_le (data + 30);
			double ceiling = array_float_le (data + 38);

			dc_sample_value_t t = {0};
			t.time = (unsigned int) s->sample_ms;
			s->callback (DC_SAMPLE_TIME, &t, s->userdata);

			dc_sample_value_t deco = {0};
			if (ceiling > 0.0) {
				deco.deco.type = DC_DECO_DECOSTOP;
				deco.deco.depth = ceiling;
				deco.deco.tts = tts;
			} else {
				deco.deco.type = DC_DECO_NDL;
				deco.deco.time = ndl > 0 ? (unsigned int) ndl : 0;
				deco.deco.tts = tts;
			}
			s->callback (DC_SAMPLE_DECO, &deco, s->userdata);
		}

		// Real-time gradient factors -> vendor kind 5 (no standard channel).
		// gf99 @186 and surface @190 sit past the cylinder array; the
		// leading-tissue GF @78 is inside that span, so it's only reliable
		// while the upper tank slots are unused (as on this reference dive).
		if (s->callback && size >= 192) {
			int gf99    = (int16_t) array_uint16_le (data + 186);
			int gf_surf = (int16_t) array_uint16_le (data + 190);
			int gf_lead = (int16_t) array_uint16_le (data + 78);
			unsigned char rec[7];
			rec[0] = VENDOR_KIND_GF;
			rec[1] = gf99 & 0xFF;    rec[2] = (gf99 >> 8) & 0xFF;
			rec[3] = gf_surf & 0xFF; rec[4] = (gf_surf >> 8) & 0xFF;
			rec[5] = gf_lead & 0xFF; rec[6] = (gf_lead >> 8) & 0xFF;
			suunto_nautic_emit_vendor (s->callback, s->userdata, s->sample_ms, rec, sizeof (rec));
		}
	} else if (id == CHUNK_GPS && size >= 18) {
		// Payload: [timeDelta:2][UTC:8 ms LE][lat:4][lon:4]. UTC is an
		// absolute UNIX time in milliseconds; subtracting this sample's
		// relative time (s->sample_ms, the monotonic clock -- a GPS fix can
		// land inside a GPS-chunk rewind window, and the raw sum there is
		// off by the excursion) yields the stream-start epoch, i.e. the
		// dive start (== the logbook id, confirmed to the second). This is
		// the only absolute clock in the stream, so the first GPS fix sets
		// the dive datetime.
		unsigned long long utc_ms = array_uint64_le (data + 2);
		int lat_raw = (int) array_uint32_le (data + 10);
		int lon_raw = (int) array_uint32_le (data + 14);

		if (!parser->have_datetime && utc_ms > (unsigned long long) s->sample_ms) {
			parser->have_datetime = 1;
			parser->datetime = (dc_ticks_t) ((utc_ms - (unsigned long long) s->sample_ms) / 1000);
		}

		if (!parser->have_location) {
			parser->have_location = 1;
			parser->location.latitude = lat_raw / 1.0e7;
			parser->location.longitude = lon_raw / 1.0e7;
			parser->location.altitude = 0.0;
		}

		if (s->callback) {
			dc_sample_value_t sample = {0};
			sample.time = (unsigned int) s->sample_ms;
			s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
			sample.location.latitude = lat_raw / 1.0e7;
			sample.location.longitude = lon_raw / 1.0e7;
			sample.location.altitude = 0.0;
			s->callback (DC_SAMPLE_LOCATION, &sample, s->userdata);
		}
	} else if (id == CHUNK_DIVE_STATE && size >= 3) {
		unsigned int new_state = data[2];
		if (new_state == DIVE_STATE_DIVING && s->dive_state != DIVE_STATE_DIVING) {
			s->diving_start_ms = s->time_ms;
		} else if (new_state != DIVE_STATE_DIVING && s->dive_state == DIVE_STATE_DIVING) {
			if (s->diving_start_ms >= 0)
				s->total_dive_ms += s->time_ms - s->diving_start_ms;
			s->diving_start_ms = -1;
		}
		s->dive_state = new_state;
	} else if ((id == CHUNK_EVENT_ALARM || id == CHUNK_EVENT_WARNING ||
			id == CHUNK_EVENT_NOTIFY || id == CHUNK_EVENT_STATE) && size >= 4) {
		// [timeDelta:2][Type:1][Active:1]; Active 1=begin, 0=end.
		// The libdivecomputer event vocabulary can't express Suunto's full
		// set (e.g. "Safety Stop Ahead" vs "At Safety Stop" both map to
		// SAFETYSTOP), so alongside the mapped type we pass the native
		// (subgroup, type) through event.value as (chunk_id << 8 | type).
		// Consumers that want the exact Suunto label decode it from there;
		// standard consumers use event.type as usual. (Gas switch keeps
		// event.value as the gas number, per libdivecomputer convention.)
		// Emit the begin edge only (Active=1). Suunto toggles these as
		// begin/end state pairs, but the end carries no diving-relevant
		// information -- the Suunto app itself shows only the begins --
		// and emitting both makes downstream apps draw a duplicate
		// marker where the condition cleared.
		if (s->callback && data[3]) {
			unsigned int mapped = suunto_nautic_map_event (id, data[2]);
			if (mapped != SAMPLE_EVENT_NONE) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) s->sample_ms;
				s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
				sample.event.type = mapped;
				sample.event.flags = SAMPLE_FLAGS_BEGIN;
				sample.event.value = (id << 8) | data[2];
				s->callback (DC_SAMPLE_EVENT, &sample, s->userdata);
			}
		}
	} else if (id == CHUNK_OOAM && size >= 3) {
		// [timeDelta:2][Type:1]; one-shot dive-end reason (Ooam.Type: Out of
		// battery / Ceiling broken / SW crash / Max depth / Algorithm changed
		// / Gauge dive). No Active byte. Emitted as a begin-edge event; the
		// native (subgroup, type) is passed in event.value = (chunk_id<<8|type)
		// for the precise Suunto label, same convention as the other events.
		if (s->callback) {
			unsigned int mapped = suunto_nautic_map_event (id, data[2]);
			if (mapped != SAMPLE_EVENT_NONE) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) s->sample_ms;
				s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
				sample.event.type = mapped;
				sample.event.flags = SAMPLE_FLAGS_BEGIN;
				sample.event.value = (id << 8) | data[2];
				s->callback (DC_SAMPLE_EVENT, &sample, s->userdata);
			}
		}
	} else if (id == CHUNK_GAS_SWITCH && size >= 4) {
		// [timeDelta:2][gasnumber:int16 LE] -- 0-based, and equal to the
		// cylinder slot / gas-mix index.
		int gasnum = (int16_t) array_uint16_le (data + 2);
		if (gasnum >= 0 && s->callback) {
			dc_sample_value_t sample = {0};
			sample.time = (unsigned int) s->sample_ms;
			s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
			// Modern channel: the active gas-mix index.
			sample.gasmix = (unsigned int) gasnum;
			s->callback (DC_SAMPLE_GASMIX, &sample, s->userdata);
			// Legacy channel, for consumers that only read events.
			sample.event.type = SAMPLE_EVENT_GASCHANGE;
			sample.event.flags = SAMPLE_FLAGS_BEGIN;
			sample.event.value = (unsigned int) gasnum;
			s->callback (DC_SAMPLE_EVENT, &sample, s->userdata);
		}
	} else if (id == CHUNK_HEARTRATE && size >= 3) {
		// [timeDelta:2][hr:uint8 bpm]. Optical wrist HR, present only on
		// the Suunto Ocean (the Nautic / Nautic S have no HR sensor, so
		// the chunk never appears there). Byte-exact against the app
		// export's per-sample HR on a real Ocean dive (66-113 bpm).
		unsigned int hr = data[2];
		if (hr && s->callback) {
			dc_sample_value_t sample = {0};
			sample.time = (unsigned int) s->sample_ms;
			s->callback (DC_SAMPLE_TIME, &sample, s->userdata);
			sample.heartbeat = hr;
			s->callback (DC_SAMPLE_HEARTBEAT, &sample, s->userdata);
		}
	} else if (id == CHUNK_BATTERY && size >= 7) {
		// Battery telemetry -> DC_SAMPLE_VENDOR kind 1.
		// (Current at data+2 is int16 but its scale isn't confirmed,
		// so it's left out for now.)
		if (s->callback) {
			unsigned int voltage_mv = array_uint16_le (data + 4);
			unsigned int charge_permille = data[6] * 10; // % -> permille
			unsigned char rec[5];
			rec[0] = VENDOR_KIND_BATTERY;
			rec[1] = voltage_mv & 0xFF;
			rec[2] = (voltage_mv >> 8) & 0xFF;
			rec[3] = charge_permille & 0xFF;
			rec[4] = (charge_permille >> 8) & 0xFF;
			suunto_nautic_emit_vendor (s->callback, s->userdata, s->sample_ms, rec, sizeof (rec));
		}
	} else if (id == CHUNK_GPS_ACCURACY && size >= 4) {
		// EHPE/EVPE are int8 deltas accumulated from zero -> DC_SAMPLE_VENDOR
		// kind 2, absolute metres.
		s->ehpe += (int8_t) data[2];
		s->evpe += (int8_t) data[3];
		if (s->ehpe < 0) s->ehpe = 0;
		if (s->evpe < 0) s->evpe = 0;
		if (s->callback) {
			unsigned int e = (unsigned int) s->ehpe, v = (unsigned int) s->evpe;
			unsigned char rec[5];
			rec[0] = VENDOR_KIND_GPS_ACCURACY;
			rec[1] = e & 0xFF; rec[2] = (e >> 8) & 0xFF;
			rec[3] = v & 0xFF; rec[4] = (v >> 8) & 0xFF;
			suunto_nautic_emit_vendor (s->callback, s->userdata, s->sample_ms, rec, sizeof (rec));
		}
	} else if ((id == CHUNK_IMU || id == CHUNK_IMU_ALT) && size >= 24) {
		// High-rate IMU: 9x int16 (accel/gyro/mag X,Y,Z) at offset 6, already
		// little-endian -> DC_SAMPLE_VENDOR kind 3, passed through verbatim.
		// Matched by shape (payload >= 24) across both id variants (0x23 on
		// 195-byte watches, 0x22 on 141-byte ones) so IMU decodes on all
		// firmware, not just the id-0x23 devices.
		if (s->callback) {
			unsigned char rec[1 + 18];
			rec[0] = VENDOR_KIND_IMU;
			memcpy (rec + 1, data + 6, 18);
			suunto_nautic_emit_vendor (s->callback, s->userdata, s->sample_ms, rec, sizeof (rec));
		}
	} else if ((id == CHUNK_DIVEROUTE_FEATURES || id == CHUNK_IMU) &&
			size >= 10 && size <= 16) {
		// DiveRouteFeatures: 5x uint16 at offset 6 -> DC_SAMPLE_VENDOR kind 4.
		// Id is 0x24 on 195-byte watches and 0x23 (small, payload 16) on
		// 141-byte ones; the payload bound (<= 16) excludes the 141-byte
		// 0x24 summary record so it is no longer misread as this channel.
		// NOT the dive route (which the watch does not store); semantics TBD.
		if (s->callback) {
			unsigned char rec[1 + 10];
			rec[0] = VENDOR_KIND_DIVEROUTE_FEATURES;
			memcpy (rec + 1, data + 6, 10);
			suunto_nautic_emit_vendor (s->callback, s->userdata, s->sample_ms, rec, sizeof (rec));
		}
	} else if (id == CHUNK_SURFACE_PRESSURE && size >= 6) {
		// 3 Float32 values at offset 2/6/10 (SurfacePressure,
		// MaxSurfacePressure, MinSurfacePressure), Pa -- offset
		// 2, not 0: like chunk 0x16's Depth field, there are 2
		// leading bytes before the data starts. DC_FIELD_ATMOSPHERIC
		// is a single ambient-pressure reading in bar, so only
		// SurfacePressure (offset 2) is used; last one logged wins.
		parser->have_atmospheric = 1;
		parser->atmospheric = array_float_le (data + 2) / 100000.0;
	} else if (id == CHUNK_ACTIVITY && size >= 3) {
		// [timeDelta:2][sportId:1][customModeId: ascii]. The sport id
		// is the app's ActivityType; 61/62 are the apnea sports, every
		// other diving id is open circuit.
		unsigned int sport = data[2];
		if (sport == SPORT_ID_FREEDIVE || sport == SPORT_ID_MERMAIDING)
			parser->divemode = DC_DIVEMODE_FREEDIVE;
		else
			parser->divemode = DC_DIVEMODE_OC;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_parser_parse (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_parser_t *parser = (suunto_nautic_parser_t *) abstract;

	// The dive is SUMMARY_SIZE(4) SUMMARY PROFILE, see the driver: the
	// /Summary section (gradient factors, gas mixes) and the profile (/Data).
	if (abstract->size < 4) {
		ERROR (abstract->context, "Unexpected dive size (%u).", abstract->size);
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int summary_size = array_uint32_le (abstract->data);
	if (summary_size > abstract->size - 4) {
		ERROR (abstract->context, "Unexpected summary size (%u %u).", summary_size, abstract->size);
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned char *summary = abstract->data + 4;
	const unsigned char *profile = abstract->data + 4 + summary_size;
	unsigned int profile_size = abstract->size - 4 - summary_size;

	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;
	parser->avgdepth = 0.0;
	parser->have_temperature = 0;
	parser->temperature_surface = 0.0;
	parser->temperature_minimum = 0.0;
	parser->temperature_maximum = 0.0;
	parser->ntanks = 0;
	memset (parser->tank, 0, sizeof (parser->tank));
	parser->have_location = 0;
	memset (&parser->location, 0, sizeof (parser->location));
	parser->have_atmospheric = 0;
	parser->atmospheric = 0.0;
	parser->have_datetime = 0;
	parser->datetime = 0;
	// Open circuit unless a CHUNK_ACTIVITY sport id says the dive was apnea.
	// The Nautic/Ocean are recreational OC computers (no CCR/SCR), and with no
	// activity chunk at all OC is the right default -- far better than the
	// dc_divemode_t zero value (freedive) a missing field leaves behind.
	parser->divemode = DC_DIVEMODE_OC;
	parser->ngasmixes = 0;
	memset (parser->gasmix, 0, sizeof (parser->gasmix));
	memset (parser->gasvolume, 0, sizeof (parser->gasvolume));
	parser->have_decomodel = 0;
	memset (&parser->decomodel, 0, sizeof (parser->decomodel));
	parser->have_ppo2max = 0;
	parser->ppo2max = 0.0;
	parser->have_salinity = 0;
	memset (&parser->salinity, 0, sizeof (parser->salinity));

	suunto_nautic_summary_t summary_state;
	memset (&summary_state, 0, sizeof (summary_state));
	summary_state.parser = parser;

	if (summary_size) {
		status = suunto_nautic_sbem_parse (abstract->context, summary, summary_size, suunto_nautic_parser_summary, &summary_state);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	suunto_nautic_profile_t s;
	memset (&s, 0, sizeof (s));
	s.parser = parser;
	s.callback = callback;
	s.userdata = userdata;
	s.dive_state = DIVE_STATE_IDLING;
	s.diving_start_ms = -1;
	for (unsigned int i = 0; i < MAX_TANKS * 2; i++)
		s.tankmap[i] = -1;
	for (unsigned int i = 0; i < MAX_TANKS; i++)
		s.p2_first[i] = -1;

	status = suunto_nautic_sbem_parse (abstract->context, profile, profile_size, suunto_nautic_parser_profile, &s);
	if (status != DC_STATUS_SUCCESS)
		return status;

	// A dive still in progress at the end of the stream closes the final span.
	if (s.dive_state == DIVE_STATE_DIVING && s.diving_start_ms >= 0)
		s.total_dive_ms += s.time_ms - s.diving_start_ms;

	// Dive time = total time in the Diving state (seconds). Fall back to the
	// full elapsed time if no DiveState markers were seen.
	if (s.total_dive_ms > 0)
		parser->divetime = (unsigned int) ((s.total_dive_ms + 500) / 1000);
	else
		parser->divetime = (unsigned int) ((s.time_ms + 500) / 1000);
	parser->avgdepth = s.depth_count ? s.depth_sum / s.depth_count : 0.0;

	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	suunto_nautic_parser_t *parser = (suunto_nautic_parser_t *) abstract;

	if (!parser->cached) {
		dc_status_t status = suunto_nautic_parser_parse (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	// The dive start is derived from the first GPS fix's absolute UTC. A dive
	// without a surface GPS fix has no absolute clock in the stream; the caller
	// falls back to the logbook id (which is that same timestamp).
	if (!parser->have_datetime)
		return DC_STATUS_UNSUPPORTED;

	if (datetime && !dc_datetime_gmtime (datetime, parser->datetime))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_nautic_parser_t *parser = (suunto_nautic_parser_t *) abstract;

	if (!parser->cached) {
		status = suunto_nautic_parser_parse (abstract, NULL, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
	}

	if (value == NULL)
		return DC_STATUS_SUCCESS;

	dc_tank_t *tank = (dc_tank_t *) value;

	switch (type) {
	case DC_FIELD_DIVETIME:
		*((unsigned int *) value) = parser->divetime;
		break;
	case DC_FIELD_MAXDEPTH:
		*((double *) value) = parser->maxdepth;
		break;
	case DC_FIELD_AVGDEPTH:
		*((double *) value) = parser->avgdepth;
		break;
	case DC_FIELD_TEMPERATURE_SURFACE:
		if (!parser->have_temperature)
			return DC_STATUS_UNSUPPORTED;
		*((double *) value) = parser->temperature_surface;
		break;
	case DC_FIELD_TEMPERATURE_MINIMUM:
		if (!parser->have_temperature)
			return DC_STATUS_UNSUPPORTED;
		*((double *) value) = parser->temperature_minimum;
		break;
	case DC_FIELD_TEMPERATURE_MAXIMUM:
		if (!parser->have_temperature)
			return DC_STATUS_UNSUPPORTED;
		*((double *) value) = parser->temperature_maximum;
		break;
	case DC_FIELD_TANK_COUNT:
		*((unsigned int *) value) = parser->ntanks;
		break;
	case DC_FIELD_TANK:
		if (flags >= MAX_TANKS || !parser->tank[flags].used)
			return DC_STATUS_INVALIDARGS;
		tank->type = DC_TANKVOLUME_NONE;
		tank->volume = 0.0;
		tank->workpressure = 0.0;
		tank->beginpressure = parser->tank[flags].beginpressure;
		tank->endpressure = parser->tank[flags].endpressure;
		tank->gasmix = DC_GASMIX_UNKNOWN;
		tank->usage = DC_USAGE_NONE;
		// Link the tank to its gas (slot index) and expose the per-gas
		// cylinder size from the /Summary when both are known.
		{
			unsigned int gi = parser->tank[flags].gasmix;
			if (gi < parser->ngasmixes) {
				tank->gasmix = gi;
				if (parser->gasvolume[gi] > 0.0) {
					tank->type = DC_TANKVOLUME_METRIC;
					tank->volume = parser->gasvolume[gi];
				}
			}
		}
		break;
	case DC_FIELD_LOCATION:
		if (!parser->have_location)
			return DC_STATUS_UNSUPPORTED;
		*((dc_location_t *) value) = parser->location;
		break;
	case DC_FIELD_ATMOSPHERIC:
		if (!parser->have_atmospheric)
			return DC_STATUS_UNSUPPORTED;
		*((double *) value) = parser->atmospheric;
		break;
	case DC_FIELD_GASMIX_COUNT:
		*((unsigned int *) value) = parser->ngasmixes;
		break;
	case DC_FIELD_GASMIX:
		if (flags >= parser->ngasmixes)
			return DC_STATUS_INVALIDARGS;
		*((dc_gasmix_t *) value) = parser->gasmix[flags];
		break;
	case DC_FIELD_DECOMODEL:
		if (!parser->have_decomodel)
			return DC_STATUS_UNSUPPORTED;
		*((dc_decomodel_t *) value) = parser->decomodel;
		break;
	case DC_FIELD_PPO2_MAX:
		if (!parser->have_ppo2max)
			return DC_STATUS_UNSUPPORTED;
		*((double *) value) = parser->ppo2max;
		break;
	case DC_FIELD_SALINITY:
		if (!parser->have_salinity)
			return DC_STATUS_UNSUPPORTED;
		*((dc_salinity_t *) value) = parser->salinity;
		break;
	case DC_FIELD_DIVEMODE:
		*((dc_divemode_t *) value) = parser->divemode;
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_nautic_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	return suunto_nautic_parser_parse (abstract, callback, userdata);
}
