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
 * Parses the SBEM0103 TLV stream produced by
 * suunto_nautic_device_download() after MDS-chunk extraction and
 * Heatshrink decompression (see suunto_nautic.c/.h). The format is:
 *
 *   [chunk id: 1 byte][length: 1 byte][value: length bytes]
 *   length == 255 means an extended 4-byte little-endian length follows
 *   immediately, before the value.
 *
 * Decoded chunks: 0x12 (1Hz absolute pressure / temperature), 0x16
 * (depth, cylinder pressures, gas time remaining, NDL, time-to-surface),
 * 0x17 (surface pressure), 0x0B (GPS), 0x0F (heart rate, Ocean only),
 * 0x08 (activity -> dive mode), plus the dynamically-assigned dive-event
 * subgroups. Chunks 0x0E (satellite
 * info) and 0x14 (battery) have fixed lengths that are used for resync
 * (see suunto_nautic_sbem_fixed_length) but map to no dc_field/dc_sample
 * and are not otherwise decoded. Chunks 0x23/0x24 are raw accelerometer /
 * gyroscope dumps for client-side dead reckoning, emitted through
 * DC_SAMPLE_VENDOR. Unknown chunk ids are skipped, so extending the
 * decoder is additive.
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

#define SBEM_MAGIC_SIZE 8

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

// Field offsets within the /Summary SBEM0103 section (relative to its
// "SBEM0103" signature), confirmed against real hardware. The dive profile
// (/Data) carries samples but not these; the driver appends the /Summary
// SBEM after the profile so this parser can expose them the standard way.
// GF low is at 0x35 and GF high at 0x33 (uint16 LE, %). This ordering yields
// valid low <= high on two devices (Nautic 35/75, Nautic S 40/85); the reverse
// gives low > high.
#define SUMMARY_GF_LOW   0x35 // uint16 LE, %
#define SUMMARY_GF_HIGH  0x33 // uint16 LE, %
#define SUMMARY_WATER_TYPE 0x3E // uint8 enum: 0 fresh, 1 EN13319, 2 salt
// Gas/cylinder records: an array starting at SUMMARY_GAS_BASE, one 45-byte
// record per configured gas. The record index equals the cylinder slot
// (GasNumber), so tank i uses gas i. Within a record: O2% at +1, He% at +2,
// the configured PO2 max (float32 LE, bar) at +5, and cylinder water capacity
// (float32 LE, m^3) at +9. Confirmed on real single- and dual-gas dives; a
// lone-gas dive stops at the first implausible slot.
#define SUMMARY_GAS_BASE   0xC7
#define SUMMARY_GAS_STRIDE 45
#define SUMMARY_GAS_O2     1  // uint8, %
#define SUMMARY_GAS_HE     2  // uint8, %
#define SUMMARY_GAS_PO2MAX 5  // float32 LE, bar
#define SUMMARY_GAS_VOLUME 9  // float32 LE, m^3

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
	// From the /Summary SBEM section appended after the profile, if present.
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

typedef struct sbem_chunk_t {
	unsigned int id;
	const unsigned char *data;
	unsigned int size;
} sbem_chunk_t;

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

// Chunk IDs whose payload length is fixed and has been confirmed
// against real captured data. Heatshrink decompression can leave
// localized artifacts in the stream (e.g. runs of a single repeated
// byte), which a strict linear TLV walk would otherwise misread as a
// chunk header -- permanently desyncing every chunk after it. Any
// candidate header naming one of these IDs is only accepted if its
// length byte matches; otherwise it is a "ghost chunk" and the parser
// resynchronizes by scanning forward one byte at a time.
static int
suunto_nautic_sbem_fixed_length (unsigned int id)
{
	switch (id) {
	case CHUNK_ACTIVITY:         return 6;
	case CHUNK_GPS:              return 20;
	case CHUNK_GPS_ACCURACY:     return 6;
	case CHUNK_BATTERY:          return 7;
	case CHUNK_SURFACE_PRESSURE: return 14;
	default:                     return -1; // unknown or variable length
	}
}

// Advance to the next TLV chunk starting at *offset. Returns 0 (and
// leaves *offset unchanged) once the buffer is exhausted.
static int
suunto_nautic_sbem_next (const unsigned char data[], unsigned int size, unsigned int *offset, sbem_chunk_t *chunk)
{
	unsigned int pos = *offset;

	while (pos + 2 <= size) {
		unsigned int id = data[pos];
		unsigned int length = data[pos + 1];
		unsigned int header = 2;

		if (length == 255) {
			if (pos + 6 > size) {
				pos++;
				continue;
			}
			length = array_uint32_le (data + pos + 2);
			header = 6;
		}

		int fixed = suunto_nautic_sbem_fixed_length (id);
		if (fixed >= 0 && (unsigned int) fixed != length) {
			// Ghost chunk: a real chunk with this id never has
			// this length. Resynchronize.
			pos++;
			continue;
		}

		if (pos + header + length > size) {
			pos++;
			continue;
		}

		chunk->id = id;
		chunk->data = data + pos + header;
		chunk->size = length;

		*offset = pos + header + length;

		return 1;
	}

	return 0;
}

// The driver appends the (uncompressed) /Summary SBEM after the profile,
// so the combined buffer holds two "SBEM0103" sections. Return the offset
// of the second one (the /Summary), or `size` when there's only the
// profile. Bounds the profile chunk walk and locates the /Summary fields.
static size_t
suunto_nautic_find_summary (const unsigned char *data, size_t size)
{
	if (size < SBEM_MAGIC_SIZE)
		return size;
	for (size_t i = SBEM_MAGIC_SIZE; i + SBEM_MAGIC_SIZE <= size; i++) {
		if (memcmp (data + i, "SBEM0103", SBEM_MAGIC_SIZE) == 0)
			return i;
	}
	return size;
}

// Parse gradient factors, gas mixes and per-gas cylinder size from the
// /Summary section, whose "SBEM0103" signature is at `sbem` (length `size`).
// Offsets are relative to that signature (confirmed on real hardware). Gases
// are validated by plausibility (O2 in 1..100, He in 0..100-O2) and counted
// until the first implausible slot, since unused slots hold unrelated bytes.
static void
suunto_nautic_parse_summary (suunto_nautic_parser_t *parser, const unsigned char *sbem, size_t size)
{
	if (size >= SUMMARY_GF_LOW + 2) { // GF_LOW (0x35) is the higher of the two offsets
		unsigned int low = array_uint16_le (sbem + SUMMARY_GF_LOW);
		unsigned int high = array_uint16_le (sbem + SUMMARY_GF_HIGH);
		parser->decomodel.type = DC_DECOMODEL_BUHLMANN;
		parser->decomodel.conservatism = 0;
		parser->decomodel.params.gf.low = low;
		parser->decomodel.params.gf.high = high;
		parser->have_decomodel = 1;
	}

	// Water type at +0x3E (uint8): 0 fresh, 1 EN13319, 2 salt.
	if (size > SUMMARY_WATER_TYPE) {
		unsigned int wt = sbem[SUMMARY_WATER_TYPE];
		if (wt <= 2) {
			static const double density[] = {1000.0, 1020.0, 1030.0};
			parser->salinity.type = wt ? DC_WATER_SALT : DC_WATER_FRESH;
			parser->salinity.density = density[wt];
			parser->have_salinity = 1;
		}
	}

	for (unsigned int i = 0; i < MAX_GASMIXES; i++) {
		size_t base = SUMMARY_GAS_BASE + (size_t) i * SUMMARY_GAS_STRIDE;
		if (base + SUMMARY_GAS_O2 >= size)
			break;
		unsigned int o2 = sbem[base + SUMMARY_GAS_O2];
		unsigned int he = sbem[base + SUMMARY_GAS_HE];
		if (o2 < 1 || o2 > 100 || he > 100 || o2 + he > 100)
			break; // unused/implausible slot -- stop
		unsigned int g = parser->ngasmixes;
		parser->gasmix[g].oxygen = o2 / 100.0;
		parser->gasmix[g].helium = he / 100.0;
		parser->gasmix[g].nitrogen = 1.0 - (o2 + he) / 100.0;
		parser->gasmix[g].usage = DC_USAGE_NONE;
		// Per-gas cylinder water capacity (float32 m^3) -> litres, when the
		// record's volume field is present and physically sane.
		if (base + SUMMARY_GAS_VOLUME + 4 <= size) {
			double vol_m3 = array_float_le (sbem + base + SUMMARY_GAS_VOLUME);
			if (vol_m3 > 0.0 && vol_m3 < 1.0)
				parser->gasvolume[g] = vol_m3 * 1000.0;
		}
		// The configured PO2 max (float32 bar) is a single watch setting held
		// in the first gas record. Record it once, when physically sane.
		if (g == 0 && base + SUMMARY_GAS_PO2MAX + 4 <= size) {
			double ppo2 = array_float_le (sbem + base + SUMMARY_GAS_PO2MAX);
			if (ppo2 >= 0.5 && ppo2 <= 3.0) {
				parser->ppo2max = ppo2;
				parser->have_ppo2max = 1;
			}
		}
		parser->ngasmixes++;
	}
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
suunto_nautic_parser_parse (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_nautic_parser_t *parser = (suunto_nautic_parser_t *) abstract;

	if (abstract->size < SBEM_MAGIC_SIZE || memcmp (abstract->data, "SBEM0103", SBEM_MAGIC_SIZE) != 0) {
		ERROR (abstract->context, "Unexpected magic in the SBEM stream.");
		return DC_STATUS_DATAFORMAT;
	}

	// The profile (/Data) is the first SBEM section; an optional /Summary
	// section (gradient factors, gas mix) is appended after it. Walk only
	// the profile here; parse /Summary separately below.
	size_t profile_size = suunto_nautic_find_summary (abstract->data, abstract->size);

	unsigned int offset = SBEM_MAGIC_SIZE;
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
	int time_ms = 0;
	int sample_ms = 0;

	// Dive phase, from CHUNK_DIVE_STATE. Dive time is the TOTAL time spent in
	// the Diving state (sum of every Diving span), which matches the app's
	// DiveTimeMax on multi-level dives that briefly surface mid-dive -- taking
	// only the longest single span undercounts those. A spurious ~0 s startup
	// blip contributes nothing. Average depth is taken over Diving samples only;
	// counting from the first raw sample would include the pre-dive/surface
	// phase and skew both low.
	unsigned int dive_state = DIVE_STATE_IDLING;
	int diving_start_ms = -1;
	int total_dive_ms = 0;

	double maxdepth = 0.0;
	double depth_sum = 0.0;
	unsigned int depth_count = 0;

	unsigned int have_temperature = 0;
	double temperature_surface = 0.0;
	double temperature_minimum = 0.0;
	double temperature_maximum = 0.0;

	unsigned int ntanks = 0;
	suunto_nautic_tank_t tank[MAX_TANKS];
	memset (tank, 0, sizeof (tank));
	// Each cylinder slot has two pressure fields: Pressure (+2, the main
	// transmitter) and Pressure2 (+6, a sidemount second transmitter). Map
	// each (slot, field) with data to a compacted tank index. Pressure2 is
	// gated: a real second transmitter produces a continuous non-zero
	// curve, whereas a slot with no second transmitter emits a single
	// spurious Pressure2 sample then zeros -- so only accept Pressure2 once
	// it has read non-zero at least twice (p2_first holds the first value).
	int tankmap[MAX_TANKS * 2];
	int p2_first[MAX_TANKS];
	for (unsigned int ti = 0; ti < MAX_TANKS * 2; ti++)
		tankmap[ti] = -1;
	for (unsigned int ti = 0; ti < MAX_TANKS; ti++)
		p2_first[ti] = -1;

	unsigned int have_location = 0;
	dc_location_t location = {0};

	unsigned int have_atmospheric = 0;
	double atmospheric = 0.0;

	unsigned int have_datetime = 0;

	// Open circuit unless a CHUNK_ACTIVITY sport id says the dive was apnea.
	// The Nautic/Ocean are recreational OC computers (no CCR/SCR), and with no
	// activity chunk at all OC is the right default -- far better than the
	// dc_divemode_t zero value (freedive) a missing field leaves behind.
	dc_divemode_t divemode = DC_DIVEMODE_OC;

	// GPS horizontal/vertical position error, int8-delta-accumulated (chunk 0x0E).
	int ehpe = 0, evpe = 0;

	sbem_chunk_t chunk;
	while (suunto_nautic_sbem_next (abstract->data, (unsigned int) profile_size, &offset, &chunk)) {
		// Advance the clock by this chunk's leading ms delta (all groups
		// except the timeline base carry one).
		if (chunk.id != CHUNK_TIMELINE_BASE && chunk.size >= 2)
			time_ms += (int16_t) array_uint16_le (chunk.data);
		if (time_ms > sample_ms)
			sample_ms = time_ms;

		if (chunk.id == CHUNK_PROFILE_1HZ && chunk.size >= 18) {
			double temperature = array_uint16_le (chunk.data + 16) / 100.0 - 273.15;

			if (!have_temperature) {
				// The first reading is taken at/near the surface at dive start.
				temperature_surface = temperature;
				temperature_minimum = temperature_maximum = temperature;
				have_temperature = 1;
			} else {
				if (temperature < temperature_minimum)
					temperature_minimum = temperature;
				if (temperature > temperature_maximum)
					temperature_maximum = temperature;
			}

			if (callback) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) sample_ms;
				callback (DC_SAMPLE_TIME, &sample, userdata);
				sample.temperature = temperature;
				callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
			}
		} else if (chunk.id == CHUNK_EXTENDED_STATUS) {
			if (chunk.size >= 6) {
				double depth = array_float_le (chunk.data + 2);

				if (depth > maxdepth)
					maxdepth = depth;
				// Average only over the Diving phase (matches the app).
				if (dive_state == DIVE_STATE_DIVING) {
					depth_sum += depth;
					depth_count++;
				}

				if (callback) {
					dc_sample_value_t sample = {0};
					sample.time = (unsigned int) sample_ms;
					callback (DC_SAMPLE_TIME, &sample, userdata);
					sample.depth = depth;
					callback (DC_SAMPLE_DEPTH, &sample, userdata);
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
					if (base + 18 > chunk.size)
						break; // full tank record doesn't fit this chunk
					if (chunk.data[base] != i)
						break; // not a real tank slot

					// Gas time remaining: uint32 LE seconds at record +10, for
					// the primary cylinder. 0xFFFFFFFF means not computed (no
					// AI, or not enough data yet). This is the app's
					// Cylinders[].GasTime; exposed as RBT minutes, the same way
					// suunto_eonsteel does.
					if (i == 0 && callback) {
						unsigned int gastime = array_uint32_le (chunk.data + base + 10);
						if (gastime != 0xFFFFFFFF && gastime != 0) {
							dc_sample_value_t sample = {0};
							sample.time = (unsigned int) sample_ms;
							callback (DC_SAMPLE_TIME, &sample, userdata);
							sample.rbt = gastime / 60;
							callback (DC_SAMPLE_RBT, &sample, userdata);
						}
					}

					for (unsigned int field = 0; field < 2; field++) {
						unsigned int pressure_pa = array_uint32_le (chunk.data + base + 2 + field * 4);
						if (pressure_pa == 0)
							continue;
						unsigned int key = i * 2 + field;
						if (field == 1 && tankmap[key] < 0) {
							// Defer creating a Pressure2 tank until its second non-zero
							// reading, so a lone spurious sample doesn't become a phantom.
							if (p2_first[i] < 0) {
								p2_first[i] = (int) pressure_pa;
								continue;
							}
						}
						double bar = pressure_pa / 100000.0;
						if (tankmap[key] < 0) {
							if (ntanks >= MAX_TANKS)
								continue;
							tankmap[key] = (int) ntanks;
							tank[ntanks].used = 1;
							// Cylinder slot index == GasNumber, so this tank
							// breathes gas i (linked to gasmix[i] below).
							tank[ntanks].gasmix = i;
							// Seed begin from the first observed reading (the deferred
							// one for Pressure2); refined to the max below.
							tank[ntanks].beginpressure = (field == 1 && p2_first[i] >= 0)
								? p2_first[i] / 100000.0 : bar;
							ntanks++;
						}
						unsigned int t = (unsigned int) tankmap[key];
						tank[t].endpressure = bar;
						// Begin pressure = the highest reading seen: a cylinder only
						// loses pressure during a dive, so the max is the true start.
						// This ignores an initial dropout where a transmitter that
						// hasn't paired yet reports a spurious low value (e.g. ~9 bar
						// for the first minutes, then jumps to the real ~200 bar).
						if (bar > tank[t].beginpressure)
							tank[t].beginpressure = bar;
						if (callback) {
							dc_sample_value_t sample = {0};
							sample.time = (unsigned int) sample_ms;
							callback (DC_SAMPLE_TIME, &sample, userdata);
							sample.pressure.tank = t;
							sample.pressure.value = bar;
							callback (DC_SAMPLE_PRESSURE, &sample, userdata);
						}
					}
				}
			}

			// Deco/safety fields. Offsets validated 244/244 against the app's
			// export: TTS uint16 @22 (s), NDL int16 @30 (s), Ceiling float32 @38
			// (m). Ceiling/NDL/TTS use the standard DC_SAMPLE_DECO channel.
			if (callback && chunk.size >= 42) {
				unsigned int tts = array_uint16_le (chunk.data + 22);
				int ndl = (int16_t) array_uint16_le (chunk.data + 30);
				double ceiling = array_float_le (chunk.data + 38);

				dc_sample_value_t t = {0};
				t.time = (unsigned int) sample_ms;
				callback (DC_SAMPLE_TIME, &t, userdata);

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
				callback (DC_SAMPLE_DECO, &deco, userdata);
			}

			// Real-time gradient factors -> vendor kind 5 (no standard channel).
			// gf99 @186 and surface @190 sit past the cylinder array; the
			// leading-tissue GF @78 is inside that span, so it's only reliable
			// while the upper tank slots are unused (as on this reference dive).
			if (callback && chunk.size >= 192) {
				int gf99    = (int16_t) array_uint16_le (chunk.data + 186);
				int gf_surf = (int16_t) array_uint16_le (chunk.data + 190);
				int gf_lead = (int16_t) array_uint16_le (chunk.data + 78);
				unsigned char rec[7];
				rec[0] = VENDOR_KIND_GF;
				rec[1] = gf99 & 0xFF;    rec[2] = (gf99 >> 8) & 0xFF;
				rec[3] = gf_surf & 0xFF; rec[4] = (gf_surf >> 8) & 0xFF;
				rec[5] = gf_lead & 0xFF; rec[6] = (gf_lead >> 8) & 0xFF;
				suunto_nautic_emit_vendor (callback, userdata, sample_ms, rec, sizeof (rec));
			}
		} else if (chunk.id == CHUNK_GPS && chunk.size >= 18) {
			// Payload: [timeDelta:2][UTC:8 ms LE][lat:4][lon:4]. UTC is an
			// absolute UNIX time in milliseconds; subtracting this sample's
			// relative time (sample_ms, the monotonic clock -- a GPS fix can
			// land inside a GPS-chunk rewind window, and the raw sum there is
			// off by the excursion) yields the stream-start epoch, i.e. the
			// dive start (== the logbook id, confirmed to the second). This is
			// the only absolute clock in the stream, so the first GPS fix sets
			// the dive datetime.
			unsigned long long utc_ms = array_uint64_le (chunk.data + 2);
			int lat_raw = (int) array_uint32_le (chunk.data + 10);
			int lon_raw = (int) array_uint32_le (chunk.data + 14);

			if (!have_datetime && utc_ms > (unsigned long long) sample_ms) {
				have_datetime = 1;
				parser->datetime = (dc_ticks_t) ((utc_ms - (unsigned long long) sample_ms) / 1000);
			}

			if (!have_location) {
				have_location = 1;
				location.latitude = lat_raw / 1.0e7;
				location.longitude = lon_raw / 1.0e7;
				location.altitude = 0.0;
			}

			if (callback) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) sample_ms;
				callback (DC_SAMPLE_TIME, &sample, userdata);
				sample.location.latitude = lat_raw / 1.0e7;
				sample.location.longitude = lon_raw / 1.0e7;
				sample.location.altitude = 0.0;
				callback (DC_SAMPLE_LOCATION, &sample, userdata);
			}
		} else if (chunk.id == CHUNK_DIVE_STATE && chunk.size >= 3) {
			unsigned int new_state = chunk.data[2];
			if (new_state == DIVE_STATE_DIVING && dive_state != DIVE_STATE_DIVING) {
				diving_start_ms = time_ms;
			} else if (new_state != DIVE_STATE_DIVING && dive_state == DIVE_STATE_DIVING) {
				if (diving_start_ms >= 0)
					total_dive_ms += time_ms - diving_start_ms;
				diving_start_ms = -1;
			}
			dive_state = new_state;
		} else if ((chunk.id == CHUNK_EVENT_ALARM || chunk.id == CHUNK_EVENT_WARNING ||
				chunk.id == CHUNK_EVENT_NOTIFY || chunk.id == CHUNK_EVENT_STATE) && chunk.size >= 4) {
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
			if (callback && chunk.data[3]) {
				unsigned int mapped = suunto_nautic_map_event (chunk.id, chunk.data[2]);
				if (mapped != SAMPLE_EVENT_NONE) {
					dc_sample_value_t sample = {0};
					sample.time = (unsigned int) sample_ms;
					callback (DC_SAMPLE_TIME, &sample, userdata);
					sample.event.type = mapped;
					sample.event.flags = SAMPLE_FLAGS_BEGIN;
					sample.event.value = (chunk.id << 8) | chunk.data[2];
					callback (DC_SAMPLE_EVENT, &sample, userdata);
				}
			}
		} else if (chunk.id == CHUNK_OOAM && chunk.size >= 3) {
			// [timeDelta:2][Type:1]; one-shot dive-end reason (Ooam.Type: Out of
			// battery / Ceiling broken / SW crash / Max depth / Algorithm changed
			// / Gauge dive). No Active byte. Emitted as a begin-edge event; the
			// native (subgroup, type) is passed in event.value = (chunk_id<<8|type)
			// for the precise Suunto label, same convention as the other events.
			if (callback) {
				unsigned int mapped = suunto_nautic_map_event (chunk.id, chunk.data[2]);
				if (mapped != SAMPLE_EVENT_NONE) {
					dc_sample_value_t sample = {0};
					sample.time = (unsigned int) sample_ms;
					callback (DC_SAMPLE_TIME, &sample, userdata);
					sample.event.type = mapped;
					sample.event.flags = SAMPLE_FLAGS_BEGIN;
					sample.event.value = (chunk.id << 8) | chunk.data[2];
					callback (DC_SAMPLE_EVENT, &sample, userdata);
				}
			}
		} else if (chunk.id == CHUNK_GAS_SWITCH && chunk.size >= 4) {
			// [timeDelta:2][gasnumber:int16 LE] -- 0-based, and equal to the
			// cylinder slot / gas-mix index.
			int gasnum = (int16_t) array_uint16_le (chunk.data + 2);
			if (gasnum >= 0 && callback) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) sample_ms;
				callback (DC_SAMPLE_TIME, &sample, userdata);
				// Modern channel: the active gas-mix index.
				sample.gasmix = (unsigned int) gasnum;
				callback (DC_SAMPLE_GASMIX, &sample, userdata);
				// Legacy channel, for consumers that only read events.
				sample.event.type = SAMPLE_EVENT_GASCHANGE;
				sample.event.flags = SAMPLE_FLAGS_BEGIN;
				sample.event.value = (unsigned int) gasnum;
				callback (DC_SAMPLE_EVENT, &sample, userdata);
			}
		} else if (chunk.id == CHUNK_HEARTRATE && chunk.size >= 3) {
			// [timeDelta:2][hr:uint8 bpm]. Optical wrist HR, present only on
			// the Suunto Ocean (the Nautic / Nautic S have no HR sensor, so
			// the chunk never appears there). Byte-exact against the app
			// export's per-sample HR on a real Ocean dive (66-113 bpm).
			unsigned int hr = chunk.data[2];
			if (hr && callback) {
				dc_sample_value_t sample = {0};
				sample.time = (unsigned int) sample_ms;
				callback (DC_SAMPLE_TIME, &sample, userdata);
				sample.heartbeat = hr;
				callback (DC_SAMPLE_HEARTBEAT, &sample, userdata);
			}
		} else if (chunk.id == CHUNK_BATTERY && chunk.size >= 7) {
			// Battery telemetry -> DC_SAMPLE_VENDOR kind 1.
			// (Current at chunk.data+2 is int16 but its scale isn't confirmed,
			// so it's left out for now.)
			if (callback) {
				unsigned int voltage_mv = array_uint16_le (chunk.data + 4);
				unsigned int charge_permille = chunk.data[6] * 10; // % -> permille
				unsigned char rec[5];
				rec[0] = VENDOR_KIND_BATTERY;
				rec[1] = voltage_mv & 0xFF;
				rec[2] = (voltage_mv >> 8) & 0xFF;
				rec[3] = charge_permille & 0xFF;
				rec[4] = (charge_permille >> 8) & 0xFF;
				suunto_nautic_emit_vendor (callback, userdata, sample_ms, rec, sizeof (rec));
			}
		} else if (chunk.id == CHUNK_GPS_ACCURACY && chunk.size >= 4) {
			// EHPE/EVPE are int8 deltas accumulated from zero -> DC_SAMPLE_VENDOR
			// kind 2, absolute metres.
			ehpe += (int8_t) chunk.data[2];
			evpe += (int8_t) chunk.data[3];
			if (ehpe < 0) ehpe = 0;
			if (evpe < 0) evpe = 0;
			if (callback) {
				unsigned int e = (unsigned int) ehpe, v = (unsigned int) evpe;
				unsigned char rec[5];
				rec[0] = VENDOR_KIND_GPS_ACCURACY;
				rec[1] = e & 0xFF; rec[2] = (e >> 8) & 0xFF;
				rec[3] = v & 0xFF; rec[4] = (v >> 8) & 0xFF;
				suunto_nautic_emit_vendor (callback, userdata, sample_ms, rec, sizeof (rec));
			}
		} else if ((chunk.id == CHUNK_IMU || chunk.id == CHUNK_IMU_ALT) && chunk.size >= 24) {
			// High-rate IMU: 9x int16 (accel/gyro/mag X,Y,Z) at offset 6, already
			// little-endian -> DC_SAMPLE_VENDOR kind 3, passed through verbatim.
			// Matched by shape (payload >= 24) across both id variants (0x23 on
			// 195-byte watches, 0x22 on 141-byte ones) so IMU decodes on all
			// firmware, not just the id-0x23 devices.
			if (callback) {
				unsigned char rec[1 + 18];
				rec[0] = VENDOR_KIND_IMU;
				memcpy (rec + 1, chunk.data + 6, 18);
				suunto_nautic_emit_vendor (callback, userdata, sample_ms, rec, sizeof (rec));
			}
		} else if ((chunk.id == CHUNK_DIVEROUTE_FEATURES || chunk.id == CHUNK_IMU) &&
				chunk.size >= 10 && chunk.size <= 16) {
			// DiveRouteFeatures: 5x uint16 at offset 6 -> DC_SAMPLE_VENDOR kind 4.
			// Id is 0x24 on 195-byte watches and 0x23 (small, payload 16) on
			// 141-byte ones; the payload bound (<= 16) excludes the 141-byte
			// 0x24 summary record so it is no longer misread as this channel.
			// NOT the dive route (which the watch does not store); semantics TBD.
			if (callback) {
				unsigned char rec[1 + 10];
				rec[0] = VENDOR_KIND_DIVEROUTE_FEATURES;
				memcpy (rec + 1, chunk.data + 6, 10);
				suunto_nautic_emit_vendor (callback, userdata, sample_ms, rec, sizeof (rec));
			}
		} else if (chunk.id == CHUNK_SURFACE_PRESSURE && chunk.size >= 6) {
			// 3 Float32 values at offset 2/6/10 (SurfacePressure,
			// MaxSurfacePressure, MinSurfacePressure), Pa -- offset
			// 2, not 0: like chunk 0x16's Depth field, there are 2
			// leading bytes before the data starts. DC_FIELD_ATMOSPHERIC
			// is a single ambient-pressure reading in bar, so only
			// SurfacePressure (offset 2) is used; last one logged wins.
			have_atmospheric = 1;
			atmospheric = array_float_le (chunk.data + 2) / 100000.0;
		} else if (chunk.id == CHUNK_ACTIVITY && chunk.size >= 3) {
			// [timeDelta:2][sportId:1][customModeId: ascii]. The sport id
			// is the app's ActivityType; 61/62 are the apnea sports, every
			// other diving id is open circuit.
			unsigned int sport = chunk.data[2];
			if (sport == SPORT_ID_FREEDIVE || sport == SPORT_ID_MERMAIDING)
				divemode = DC_DIVEMODE_FREEDIVE;
			else
				divemode = DC_DIVEMODE_OC;
		}
	}

	// A dive still in progress at the end of the stream closes the final span.
	if (dive_state == DIVE_STATE_DIVING && diving_start_ms >= 0)
		total_dive_ms += time_ms - diving_start_ms;

	// Dive time = total time in the Diving state (seconds). Fall back to the
	// full elapsed time if no DiveState markers were seen.
	if (total_dive_ms > 0)
		parser->divetime = (unsigned int) ((total_dive_ms + 500) / 1000);
	else
		parser->divetime = (unsigned int) ((time_ms + 500) / 1000);
	parser->maxdepth = maxdepth;
	parser->avgdepth = depth_count ? depth_sum / depth_count : 0.0;
	parser->have_temperature = have_temperature;
	parser->temperature_surface = temperature_surface;
	parser->temperature_minimum = temperature_minimum;
	parser->temperature_maximum = temperature_maximum;
	parser->ntanks = ntanks;
	memcpy (parser->tank, tank, sizeof (tank));
	parser->have_location = have_location;
	parser->location = location;
	parser->have_atmospheric = have_atmospheric;
	parser->atmospheric = atmospheric;
	parser->have_datetime = have_datetime;
	parser->divemode = divemode;

	// Gradient factors, gas mixes and per-gas cylinder size from the appended
	// /Summary section.
	parser->ngasmixes = 0;
	parser->have_decomodel = 0;
	parser->have_ppo2max = 0;
	parser->ppo2max = 0.0;
	parser->have_salinity = 0;
	memset (&parser->salinity, 0, sizeof (parser->salinity));
	memset (parser->gasmix, 0, sizeof (parser->gasmix));
	memset (parser->gasvolume, 0, sizeof (parser->gasvolume));
	memset (&parser->decomodel, 0, sizeof (parser->decomodel));
	if (profile_size < abstract->size) {
		suunto_nautic_parse_summary (parser, abstract->data + profile_size,
			abstract->size - profile_size);
	}

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
