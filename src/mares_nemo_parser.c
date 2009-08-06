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
#include <assert.h>

#include "mares_nemo.h"
#include "parser-private.h"
#include "units.h"
#include "utils.h"
#include "array.h"

typedef struct mares_nemo_parser_t mares_nemo_parser_t;

struct mares_nemo_parser_t {
	parser_t base;
};

static parser_status_t mares_nemo_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size);
static parser_status_t mares_nemo_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata);
static parser_status_t mares_nemo_parser_destroy (parser_t *abstract);

static const parser_backend_t mares_nemo_parser_backend = {
	PARSER_TYPE_MARES_NEMO,
	mares_nemo_parser_set_data, /* set_data */
	mares_nemo_parser_samples_foreach, /* samples_foreach */
	mares_nemo_parser_destroy /* destroy */
};


static int
parser_is_mares_nemo (parser_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_nemo_parser_backend;
}


parser_status_t
mares_nemo_parser_create (parser_t **out)
{
	if (out == NULL)
		return PARSER_STATUS_ERROR;

	// Allocate memory.
	mares_nemo_parser_t *parser = (mares_nemo_parser_t *) malloc (sizeof (mares_nemo_parser_t));
	if (parser == NULL) {
		WARNING ("Failed to allocate memory.");
		return PARSER_STATUS_MEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, &mares_nemo_parser_backend);

	*out = (parser_t*) parser;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_nemo_parser_destroy (parser_t *abstract)
{
	if (! parser_is_mares_nemo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	// Free memory.
	free (abstract);

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_nemo_parser_set_data (parser_t *abstract, const unsigned char *data, unsigned int size)
{
	if (! parser_is_mares_nemo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	return PARSER_STATUS_SUCCESS;
}


static parser_status_t
mares_nemo_parser_samples_foreach (parser_t *abstract, sample_callback_t callback, void *userdata)
{
	if (! parser_is_mares_nemo (abstract))
		return PARSER_STATUS_TYPE_MISMATCH;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 5)
		return PARSER_STATUS_ERROR;

	unsigned int length = array_uint16_le (data);
	assert (length <= size);

	unsigned int mode = data[length - 1];

	unsigned int nsamples = array_uint16_le (data + length - 3);

	if (mode != 2) {
		unsigned int time = 0;
		for (unsigned int i = 0; i < nsamples; ++i) {
			parser_sample_value_t sample = {0};

			unsigned int idx = 2 + 2 * i;
			unsigned int value = array_uint16_le (data + idx);
			unsigned int depth = value & 0x0FFF;
			unsigned int ascent = (value & 0xC000) >> 14;
			unsigned int violation = (value & 0x2000) >> 13;
			unsigned int deco = (value & 0x1000) >> 12;

			// Time (seconds).
			time += 20;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			// Ascent rate
			if (ascent) {
				sample.event.type = SAMPLE_EVENT_ASCENT;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = ascent;
				if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
			}

			// Deco violation
			if (violation) {
				sample.event.type = SAMPLE_EVENT_CEILING;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
			}

			// Deco stop
			if (deco) {
				sample.event.type = SAMPLE_EVENT_DECOSTOP;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = 0;
				if (callback) callback (SAMPLE_TYPE_EVENT, sample, userdata);
			}
		}
	} else {
		// A freedive session contains only summaries for each individual
		// freedive. The detailed profile data (if present) is stored after
		// the normal dive data. We assume a freedive has a detailed profile
		// when the buffer contains more data than the size indicated in the
		// header.
		int profiles = (size > length);

		unsigned int time = 0;
		unsigned int offset = length;
		for (unsigned int i = 0; i < nsamples; ++i) {
			parser_sample_value_t sample = {0};

			unsigned int idx = 2 + 6 * i;
			unsigned int maxdepth = array_uint16_le (data + idx);
			unsigned int divetime = data[idx + 2] + data[idx + 3] * 60;
			unsigned int surftime = data[idx + 4] + data[idx + 5] * 60;

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);

			if (profiles) {
				// Calculate the number of samples that should be present
				// in the profile data, based on the divetime in the summary.
				unsigned int n = (divetime + 3) / 4;

				// The last sample interval can be smaller than the normal
				// 4 seconds. We keep track of the maximum divetime, to be
				// able to adjust that last sample interval.
				unsigned int maxtime = time + divetime;

				// Process all depth samples. Once a zero depth sample is
				// reached, the current freedive profile is complete.
				unsigned int count = 0;
				while (offset + 2 <= size) {
					unsigned int depth = array_uint16_le (data + offset);
					offset += 2;

					if (depth == 0)
						break;

					count++;
					assert (count <= n);

					// Time (seconds).
					time += 4;
					if (time > maxtime)
						time = maxtime; // Adjust the last sample.
					sample.time = time;
					if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

					// Depth (1/10 m).
					sample.depth = depth / 10.0;
					if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
				}

				// Verify that the number of samples in the profile data
				// equals the predicted number of samples (from the divetime
				// in the summary entry). If both values are different, the
				// the profile data is probably incorrect.
				assert (count == n);

			} else {
				// Dive Time (seconds).
				time += divetime;
				sample.time = time;
				if (callback) callback (SAMPLE_TYPE_TIME, sample, userdata);

				// Maximum Depth (1/10 m).
				sample.depth = maxdepth / 10.0;
				if (callback) callback (SAMPLE_TYPE_DEPTH, sample, userdata);
			}
		}
		assert (offset == size);
	}

	return PARSER_STATUS_SUCCESS;
}
