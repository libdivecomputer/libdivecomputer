/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <stdio.h>	// fopen, fwrite, fclose
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
#define DC_TICKS_FORMAT "%I64d"
#else
#define DC_TICKS_FORMAT "%lld"
#endif

#include <suunto.h>
#include <reefnet.h>
#include <uwatec.h>
#include <oceanic.h>
#include <mares.h>
#include <hw.h>
#include <cressi.h>
#include <zeagle.h>
#include <atomics.h>
#include <utils.h>

#include "common.h"

static const char *g_cachedir = NULL;
static int g_cachedir_read = 1;

typedef struct device_data_t {
	device_type_t backend;
	device_devinfo_t devinfo;
	device_clock_t clock;
} device_data_t;

typedef struct dive_data_t {
	device_data_t *devdata;
	FILE* fp;
	unsigned int number;
	dc_buffer_t *fingerprint;
} dive_data_t;

typedef struct sample_data_t {
	FILE* fp;
	unsigned int nsamples;
} sample_data_t;

typedef struct backend_table_t {
	const char *name;
	device_type_t type;
} backend_table_t;

static const backend_table_t g_backends[] = {
	{"solution",	DEVICE_TYPE_SUUNTO_SOLUTION},
	{"eon",			DEVICE_TYPE_SUUNTO_EON},
	{"vyper",		DEVICE_TYPE_SUUNTO_VYPER},
	{"vyper2",		DEVICE_TYPE_SUUNTO_VYPER2},
	{"d9",			DEVICE_TYPE_SUUNTO_D9},
	{"aladin",		DEVICE_TYPE_UWATEC_ALADIN},
	{"memomouse",	DEVICE_TYPE_UWATEC_MEMOMOUSE},
	{"smart",		DEVICE_TYPE_UWATEC_SMART},
	{"sensus",		DEVICE_TYPE_REEFNET_SENSUS},
	{"sensuspro",	DEVICE_TYPE_REEFNET_SENSUSPRO},
	{"sensusultra",	DEVICE_TYPE_REEFNET_SENSUSULTRA},
	{"vtpro",		DEVICE_TYPE_OCEANIC_VTPRO},
	{"veo250",		DEVICE_TYPE_OCEANIC_VEO250},
	{"atom2",		DEVICE_TYPE_OCEANIC_ATOM2},
	{"nemo",		DEVICE_TYPE_MARES_NEMO},
	{"puck",		DEVICE_TYPE_MARES_PUCK},
	{"darwin",      DEVICE_TYPE_MARES_DARWIN},
	{"iconhd",		DEVICE_TYPE_MARES_ICONHD},
	{"ostc",		DEVICE_TYPE_HW_OSTC},
	{"edy",			DEVICE_TYPE_CRESSI_EDY},
	{"n2ition3",	DEVICE_TYPE_ZEAGLE_N2ITION3},
	{"cobalt",		DEVICE_TYPE_ATOMICS_COBALT}
};

static device_type_t
lookup_type (const char *name)
{
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		if (strcmp (name, g_backends[i].name) == 0)
			return g_backends[i].type;
	}

	return DEVICE_TYPE_NULL;
}

static const char *
lookup_name (device_type_t type)
{
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		if (g_backends[i].type == type)
			return g_backends[i].name;
	}

	return NULL;
}

static unsigned char
hex2dec (unsigned char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	else if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	else if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	else
		return 0;
}

static dc_buffer_t *
fpconvert (const char *fingerprint)
{
	// Get the length of the fingerprint data.
	size_t nbytes = (fingerprint ? strlen (fingerprint) / 2 : 0);
	if (nbytes == 0)
		return NULL;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (nbytes);

	// Convert the hexadecimal string.
	for (unsigned int i = 0; i < nbytes; ++i) {
		unsigned char msn = hex2dec (fingerprint[i * 2 + 0]);
		unsigned char lsn = hex2dec (fingerprint[i * 2 + 1]);
		unsigned char byte = (msn << 4) + lsn;

		dc_buffer_append (buffer, &byte, 1);
	}

	return buffer;
}

static dc_buffer_t *
fpread (const char *dirname, device_type_t backend, unsigned int serial)
{
	// Build the filename.
	char filename[1024] = {0};
	snprintf (filename, sizeof (filename), "%s/%s-%08X.bin",
		dirname, lookup_name (backend), serial);

	// Open the fingerprint file.
	FILE *fp = fopen (filename, "rb");
	if (fp == NULL)
		return NULL;

	// Allocate a memory buffer.
	dc_buffer_t *buffer = dc_buffer_new (0);

	// Read the entire file into the buffer.
	size_t n = 0;
	unsigned char block[1024] = {0};
	while ((n = fread (block, 1, sizeof (block), fp)) > 0) {
		dc_buffer_append (buffer, block, n);
	}

	// Close the file.
	fclose (fp);

	return buffer;
}

static void
fpwrite (dc_buffer_t *buffer, const char *dirname, device_type_t backend, unsigned int serial)
{
	// Check the buffer size.
	if (dc_buffer_get_size (buffer) == 0)
		return;

	// Build the filename.
	char filename[1024] = {0};
	snprintf (filename, sizeof (filename), "%s/%s-%08X.bin",
		dirname, lookup_name (backend), serial);

	// Open the fingerprint file.
	FILE *fp = fopen (filename, "wb");
	if (fp == NULL)
		return;

	// Write the fingerprint data.
	fwrite (dc_buffer_get_data (buffer), 1, dc_buffer_get_size (buffer), fp);

	// Close the file.
	fclose (fp);
}

volatile sig_atomic_t g_cancel = 0;

void
sighandler (int signum)
{
#ifndef _WIN32
	// Restore the default signal handler.
	signal (signum, SIG_DFL);
#endif

	g_cancel = 1;
}

static int
cancel_cb (void *userdata)
{
	return g_cancel;
}

void
sample_cb (parser_sample_type_t type, parser_sample_value_t value, void *userdata)
{
	static const char *events[] = {
		"none", "deco", "rbt", "ascent", "ceiling", "workload", "transmitter",
		"violation", "bookmark", "surface", "safety stop", "gaschange",
		"safety stop (voluntary)", "safety stop (mandatory)", "deepstop",
		"ceiling (safety stop)", "unknown", "divetime", "maxdepth",
		"OLF", "PO2", "airtime", "rgbm", "heading", "tissue level warning"};

	sample_data_t *sampledata = (sample_data_t *) userdata;

	switch (type) {
	case SAMPLE_TYPE_TIME:
		if (sampledata->nsamples++)
			fprintf (sampledata->fp, "</sample>\n");
		fprintf (sampledata->fp, "<sample>\n");
		fprintf (sampledata->fp, "   <time>%02u:%02u</time>\n", value.time / 60, value.time % 60);
		break;
	case SAMPLE_TYPE_DEPTH:
		fprintf (sampledata->fp, "   <depth>%.2f</depth>\n", value.depth);
		break;
	case SAMPLE_TYPE_PRESSURE:
		fprintf (sampledata->fp, "   <pressure tank=\"%u\">%.2f</pressure>\n", value.pressure.tank, value.pressure.value);
		break;
	case SAMPLE_TYPE_TEMPERATURE:
		fprintf (sampledata->fp, "   <temperature>%.2f</temperature>\n", value.temperature);
		break;
	case SAMPLE_TYPE_EVENT:
		fprintf (sampledata->fp, "   <event type=\"%u\" time=\"%u\" flags=\"%u\" value=\"%u\">%s</event>\n",
			value.event.type, value.event.time, value.event.flags, value.event.value, events[value.event.type]);
		break;
	case SAMPLE_TYPE_RBT:
		fprintf (sampledata->fp, "   <rbt>%u</rbt>\n", value.rbt);
		break;
	case SAMPLE_TYPE_HEARTBEAT:
		fprintf (sampledata->fp, "   <heartbeat>%u</heartbeat>\n", value.heartbeat);
		break;
	case SAMPLE_TYPE_BEARING:
		fprintf (sampledata->fp, "   <bearing>%u</bearing>\n", value.bearing);
		break;
	case SAMPLE_TYPE_VENDOR:
		fprintf (sampledata->fp, "   <vendor type=\"%u\" size=\"%u\">", value.vendor.type, value.vendor.size);
		for (unsigned int i = 0; i < value.vendor.size; ++i)
			fprintf (sampledata->fp, "%02X", ((unsigned char *) value.vendor.data)[i]);
		fprintf (sampledata->fp, "</vendor>\n");
		break;
	default:
		break;
	}
}

static parser_status_t
doparse (FILE *fp, device_data_t *devdata, const unsigned char data[], unsigned int size)
{
	// Create the parser.
	message ("Creating the parser.\n");
	parser_t *parser = NULL;
	parser_status_t rc = PARSER_STATUS_SUCCESS;
	switch (devdata->backend) {
	case DEVICE_TYPE_SUUNTO_SOLUTION:
		rc = suunto_solution_parser_create (&parser);
		break;
	case DEVICE_TYPE_SUUNTO_EON:
		rc = suunto_eon_parser_create (&parser, 0);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER:
		if (devdata->devinfo.model == 0x01)
			rc = suunto_eon_parser_create (&parser, 1);
		else
			rc = suunto_vyper_parser_create (&parser);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER2:
	case DEVICE_TYPE_SUUNTO_D9:
		rc = suunto_d9_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_UWATEC_ALADIN:
	case DEVICE_TYPE_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_parser_create (&parser, devdata->clock.devtime, devdata->clock.systime);
		break;
	case DEVICE_TYPE_UWATEC_SMART:
		rc = uwatec_smart_parser_create (&parser, devdata->devinfo.model, devdata->clock.devtime, devdata->clock.systime);
		break;
	case DEVICE_TYPE_REEFNET_SENSUS:
		rc = reefnet_sensus_parser_create (&parser, devdata->clock.devtime, devdata->clock.systime);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_parser_create (&parser, devdata->clock.devtime, devdata->clock.systime);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_parser_create (&parser, devdata->clock.devtime, devdata->clock.systime);
		break;
	case DEVICE_TYPE_OCEANIC_VTPRO:
		rc = oceanic_vtpro_parser_create (&parser);
		break;
	case DEVICE_TYPE_OCEANIC_VEO250:
		rc = oceanic_veo250_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_OCEANIC_ATOM2:
		rc = oceanic_atom2_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_MARES_NEMO:
	case DEVICE_TYPE_MARES_PUCK:
		rc = mares_nemo_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_MARES_DARWIN:
		rc = mares_darwin_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_MARES_ICONHD:
		rc = mares_iconhd_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_HW_OSTC:
		rc = hw_ostc_parser_create (&parser);
		break;
	case DEVICE_TYPE_CRESSI_EDY:
	case DEVICE_TYPE_ZEAGLE_N2ITION3:
		rc = cressi_edy_parser_create (&parser, devdata->devinfo.model);
		break;
	case DEVICE_TYPE_ATOMICS_COBALT:
		rc = atomics_cobalt_parser_create (&parser);
		break;
	default:
		rc = PARSER_STATUS_ERROR;
		break;
	}
	if (rc != PARSER_STATUS_SUCCESS) {
		WARNING ("Error creating the parser.");
		return rc;
	}

	// Register the data.
	message ("Registering the data.\n");
	rc = parser_set_data (parser, data, size);
	if (rc != PARSER_STATUS_SUCCESS) {
		WARNING ("Error registering the data.");
		parser_destroy (parser);
		return rc;
	}

	// Parse the datetime.
	message ("Parsing the datetime.\n");
	dc_datetime_t dt = {0};
	rc = parser_get_datetime (parser, &dt);
	if (rc != PARSER_STATUS_SUCCESS && rc != PARSER_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the datetime.");
		parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<datetime>%04i-%02i-%02i %02i:%02i:%02i</datetime>\n",
		dt.year, dt.month, dt.day,
		dt.hour, dt.minute, dt.second);

	// Parse the divetime.
	message ("Parsing the divetime.\n");
	unsigned int divetime = 0;
	rc = parser_get_field (parser, FIELD_TYPE_DIVETIME, 0, &divetime);
	if (rc != PARSER_STATUS_SUCCESS && rc != PARSER_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the divetime.");
		parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<divetime>%02u:%02u</divetime>\n",
		divetime / 60, divetime % 60);

	// Parse the maxdepth.
	message ("Parsing the maxdepth.\n");
	double maxdepth = 0.0;
	rc = parser_get_field (parser, FIELD_TYPE_MAXDEPTH, 0, &maxdepth);
	if (rc != PARSER_STATUS_SUCCESS && rc != PARSER_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the maxdepth.");
		parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<maxdepth>%.2f</maxdepth>\n",
		maxdepth);

	// Parse the gas mixes.
	message ("Parsing the gas mixes.\n");
	unsigned int ngases = 0;
	rc = parser_get_field (parser, FIELD_TYPE_GASMIX_COUNT, 0, &ngases);
	if (rc != PARSER_STATUS_SUCCESS && rc != PARSER_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the gas mix count.");
		parser_destroy (parser);
		return rc;
	}

	for (unsigned int i = 0; i < ngases; ++i) {
		gasmix_t gasmix = {0};
		rc = parser_get_field (parser, FIELD_TYPE_GASMIX, i, &gasmix);
		if (rc != PARSER_STATUS_SUCCESS && rc != PARSER_STATUS_UNSUPPORTED) {
			WARNING ("Error parsing the gas mix.");
			parser_destroy (parser);
			return rc;
		}

		fprintf (fp,
			"<gasmix>\n"
			"   <he>%.1f</he>\n"
			"   <o2>%.1f</o2>\n"
			"   <n2>%.1f</n2>\n"
			"</gasmix>\n",
			gasmix.helium * 100.0,
			gasmix.oxygen * 100.0,
			gasmix.nitrogen * 100.0);
	}

	// Initialize the sample data.
	sample_data_t sampledata = {0};
	sampledata.nsamples = 0;
	sampledata.fp = fp;

	// Parse the sample data.
	message ("Parsing the sample data.\n");
	rc = parser_samples_foreach (parser, sample_cb, &sampledata);
	if (rc != PARSER_STATUS_SUCCESS) {
		WARNING ("Error parsing the sample data.");
		parser_destroy (parser);
		return rc;
	}

	if (sampledata.nsamples)
		fprintf (fp, "</sample>\n");

	// Destroy the parser.
	message ("Destroying the parser.\n");
	rc = parser_destroy (parser);
	if (rc != PARSER_STATUS_SUCCESS) {
		WARNING ("Error destroying the parser.");
		return rc;
	}

	return PARSER_STATUS_SUCCESS;
}

static void
event_cb (device_t *device, device_event_t event, const void *data, void *userdata)
{
	const device_progress_t *progress = (device_progress_t *) data;
	const device_devinfo_t *devinfo = (device_devinfo_t *) data;
	const device_clock_t *clock = (device_clock_t *) data;

	device_data_t *devdata = (device_data_t *) userdata;

	switch (event) {
	case DEVICE_EVENT_WAITING:
		message ("Event: waiting for user action\n");
		break;
	case DEVICE_EVENT_PROGRESS:
		message ("Event: progress %3.2f%% (%u/%u)\n",
			100.0 * (double) progress->current / (double) progress->maximum,
			progress->current, progress->maximum);
		break;
	case DEVICE_EVENT_DEVINFO:
		devdata->devinfo = *devinfo;
		message ("Event: model=%u (0x%08x), firmware=%u (0x%08x), serial=%u (0x%08x)\n",
			devinfo->model, devinfo->model,
			devinfo->firmware, devinfo->firmware,
			devinfo->serial, devinfo->serial);
		if (g_cachedir && g_cachedir_read) {
			dc_buffer_t *fingerprint = fpread (g_cachedir, devdata->backend, devinfo->serial);
			device_set_fingerprint (device,
				dc_buffer_get_data (fingerprint),
				dc_buffer_get_size (fingerprint));
			dc_buffer_free (fingerprint);
		}
		break;
	case DEVICE_EVENT_CLOCK:
		devdata->clock = *clock;
		message ("Event: systime=" DC_TICKS_FORMAT ", devtime=%u\n",
			clock->systime, clock->devtime);
		break;
	default:
		break;
	}
}

static int
dive_cb (const unsigned char *data, unsigned int size, const unsigned char *fingerprint, unsigned int fsize, void *userdata)
{
	dive_data_t *divedata = (dive_data_t *) userdata;

	divedata->number++;

	message ("Dive: number=%u, size=%u, fingerprint=", divedata->number, size);
	for (unsigned int i = 0; i < fsize; ++i)
		message ("%02X", fingerprint[i]);
	message ("\n");

	if (divedata->number == 1) {
		divedata->fingerprint = dc_buffer_new (fsize);
		dc_buffer_append (divedata->fingerprint, fingerprint, fsize);
	}

	if (divedata->fp) {
		fprintf (divedata->fp, "<dive>\n<number>%u</number>\n<size>%u</size>\n<fingerprint>", divedata->number, size);
		for (unsigned int i = 0; i < fsize; ++i)
			fprintf (divedata->fp, "%02X", fingerprint[i]);
		fprintf (divedata->fp, "</fingerprint>\n");

		doparse (divedata->fp, divedata->devdata, data, size);

		fprintf (divedata->fp, "</dive>\n");
	}

	return 1;
}


static void
usage (const char *filename)
{
#ifndef _MSC_VER
	fprintf (stderr, "Usage:\n\n");
	fprintf (stderr, "   %s [options] devname\n\n", filename);
	fprintf (stderr, "Options:\n\n");
	fprintf (stderr, "   -b name        Set backend name (required).\n");
	fprintf (stderr, "   -t model       Set model code.\n");
	fprintf (stderr, "   -f hexdata     Set fingerprint data.\n");
	fprintf (stderr, "   -l logfile     Set logfile.\n");
	fprintf (stderr, "   -d filename    Download dives.\n");
	fprintf (stderr, "   -m filename    Download memory dump.\n");
	fprintf (stderr, "   -c cachedir    Set cache directory.\n");
	fprintf (stderr, "   -h             Show this help message.\n\n");
#else
	fprintf (stderr, "Usage:\n\n");
	fprintf (stderr, "   %s backend devname\n\n", filename);
#endif

	fprintf (stderr, "Supported backends:\n\n");
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		fprintf (stderr, "%s", g_backends[i].name);
		if (i != nbackends - 1)
			fprintf (stderr, ", ");
		else
			fprintf (stderr, "\n\n");
	}
}


static device_status_t
dowork (device_type_t backend, unsigned int model, const char *devname, const char *rawfile, const char *xmlfile, int memory, int dives, dc_buffer_t *fingerprint)
{
	device_status_t rc = DEVICE_STATUS_SUCCESS;

	// Initialize the device data.
	device_data_t devdata = {0};
	devdata.backend = backend;

	// Open the device.
	message ("Opening the device (%s, %s).\n",
		lookup_name (backend), devname ? devname : "null");
	device_t *device = NULL;
	switch (backend) {
	case DEVICE_TYPE_SUUNTO_SOLUTION:
		rc = suunto_solution_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_EON:
		rc = suunto_eon_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER:
		rc = suunto_vyper_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER2:
		rc = suunto_vyper2_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_D9:
		rc = suunto_d9_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_ALADIN:
		rc = uwatec_aladin_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_SMART:
		rc = uwatec_smart_device_open (&device);
		break;
	case DEVICE_TYPE_REEFNET_SENSUS:
		rc = reefnet_sensus_device_open (&device, devname);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_device_open (&device, devname);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_VTPRO:
		rc = oceanic_vtpro_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_VEO250:
		rc = oceanic_veo250_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_ATOM2:
		rc = oceanic_atom2_device_open (&device, devname);
		break;
	case DEVICE_TYPE_MARES_NEMO:
		rc = mares_nemo_device_open (&device, devname);
		break;
	case DEVICE_TYPE_MARES_PUCK:
		rc = mares_puck_device_open (&device, devname);
		break;
	case DEVICE_TYPE_MARES_DARWIN:
		rc = mares_darwin_device_open (&device, devname, model);
		break;
	case DEVICE_TYPE_MARES_ICONHD:
		rc = mares_iconhd_device_open (&device, devname);
		break;
	case DEVICE_TYPE_HW_OSTC:
		rc = hw_ostc_device_open (&device, devname);
		break;
	case DEVICE_TYPE_CRESSI_EDY:
		rc = cressi_edy_device_open (&device, devname);
		break;
	case DEVICE_TYPE_ZEAGLE_N2ITION3:
		rc = zeagle_n2ition3_device_open (&device, devname);
		break;
	case DEVICE_TYPE_ATOMICS_COBALT:
		rc = atomics_cobalt_device_open (&device);
		break;
	default:
		rc = DEVICE_STATUS_ERROR;
		break;
	}
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening device.");
		return rc;
	}

	// Register the event handler.
	message ("Registering the event handler.\n");
	int events = DEVICE_EVENT_WAITING | DEVICE_EVENT_PROGRESS | DEVICE_EVENT_DEVINFO | DEVICE_EVENT_CLOCK;
	rc = device_set_events (device, events, event_cb, &devdata);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error registering the event handler.");
		device_close (device);
		return rc;
	}

	// Register the cancellation handler.
	message ("Registering the cancellation handler.\n");
	rc = device_set_cancel (device, cancel_cb, NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error registering the cancellation handler.");
		device_close (device);
		return rc;
	}

	// Register the fingerprint data.
	if (fingerprint) {
		message ("Registering the fingerprint data.\n");
		rc = device_set_fingerprint (device, dc_buffer_get_data (fingerprint), dc_buffer_get_size (fingerprint));
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Error registering the fingerprint data.");
			device_close (device);
			return rc;
		}
	}

	if (memory) {
		// Allocate a memory buffer.
		dc_buffer_t *buffer = dc_buffer_new (0);

		// Download the memory dump.
		message ("Downloading the memory dump.\n");
		rc = device_dump (device, buffer);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Error downloading the memory dump.");
			dc_buffer_free (buffer);
			device_close (device);
			return rc;
		}

		// Write the memory dump to disk.
		FILE* fp = fopen (rawfile, "wb");
		if (fp != NULL) {
			fwrite (dc_buffer_get_data (buffer), 1, dc_buffer_get_size (buffer), fp);
			fclose (fp);
		}

		// Free the memory buffer.
		dc_buffer_free (buffer);
	}

	if (dives) {
		// Initialize the dive data.
		dive_data_t divedata = {0};
		divedata.devdata = &devdata;
		divedata.fingerprint = NULL;
		divedata.number = 0;

		// Open the output file.
		divedata.fp = fopen (xmlfile, "w");

		// Download the dives.
		message ("Downloading the dives.\n");
		rc = device_foreach (device, dive_cb, &divedata);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Error downloading the dives.");
			dc_buffer_free (divedata.fingerprint);
			if (divedata.fp) fclose (divedata.fp);
			device_close (device);
			return rc;
		}

		// Store the fingerprint data.
		if (g_cachedir) {
			fpwrite (divedata.fingerprint, g_cachedir, devdata.backend, devdata.devinfo.serial);
		}

		// Free the fingerprint buffer.
		dc_buffer_free (divedata.fingerprint);

		// Close the output file.
		if (divedata.fp) fclose (divedata.fp);
	}

	// Close the device.
	message ("Closing the device.\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error closing the device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


int
main (int argc, char *argv[])
{
	// Default values.
	device_type_t backend = DEVICE_TYPE_NULL;
	const char *logfile = "output.log";
	const char *rawfile = "output.bin";
	const char *xmlfile = "output.xml";
	const char *devname = NULL;
	const char *fingerprint = NULL;
	unsigned int model = 0;
	int memory = 0, dives = 0;

#ifndef _MSC_VER
	// Parse command-line options.
	int opt = 0;
	while ((opt = getopt (argc, argv, "b:t:f:l:m:d:c:h")) != -1) {
		switch (opt) {
		case 'b':
			backend = lookup_type (optarg);
			break;
		case 't':
			model = strtoul (optarg, NULL, 0);
			break;
		case 'f':
			fingerprint = optarg;
			g_cachedir_read = 0;
			break;
		case 'l':
			logfile = optarg;
			break;
		case 'm':
			memory = 1;
			rawfile = optarg;
			break;
		case 'd':
			dives = 1;
			xmlfile = optarg;
			break;
		case 'c':
			g_cachedir = optarg;
			break;
		case '?':
		case 'h':
		default:
			usage (argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		devname = argv[optind];
#else
	if (argc > 1)
		backend = lookup_type (argv[1]);

	if (argc > 2)
		devname = argv[2];
#endif

	// Set the default action.
	if (!memory && !dives) {
		memory = 1;
		dives = 1;
	}

	// The backend is a mandatory argument.
	if (backend == DEVICE_TYPE_NULL) {
		usage (argv[0]);
		return EXIT_FAILURE;
	}

	signal (SIGINT, sighandler);

	message_set_logfile (logfile);

	dc_buffer_t *fp = fpconvert (fingerprint);
	device_status_t rc = dowork (backend, model, devname, rawfile, xmlfile, memory, dives, fp);
	dc_buffer_free (fp);
	message ("Result: %s\n", errmsg (rc));

	message_set_logfile (NULL);

	return rc != DEVICE_STATUS_SUCCESS ? EXIT_FAILURE : EXIT_SUCCESS;
}
