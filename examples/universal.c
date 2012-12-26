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

#ifdef _MSC_VER
#define snprintf _snprintf
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#ifdef _WIN32
#define DC_TICKS_FORMAT "%I64d"
#else
#define DC_TICKS_FORMAT "%lld"
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>

#include "utils.h"
#include "common.h"

static const char *g_cachedir = NULL;
static int g_cachedir_read = 1;

typedef struct device_data_t {
	dc_event_devinfo_t devinfo;
	dc_event_clock_t clock;
} device_data_t;

typedef struct dive_data_t {
	dc_device_t *device;
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
	dc_family_t type;
} backend_table_t;

static const backend_table_t g_backends[] = {
	{"solution",    DC_FAMILY_SUUNTO_SOLUTION},
	{"eon",	        DC_FAMILY_SUUNTO_EON},
	{"vyper",       DC_FAMILY_SUUNTO_VYPER},
	{"vyper2",      DC_FAMILY_SUUNTO_VYPER2},
	{"d9",          DC_FAMILY_SUUNTO_D9},
	{"aladin",      DC_FAMILY_UWATEC_ALADIN},
	{"memomouse",   DC_FAMILY_UWATEC_MEMOMOUSE},
	{"smart",       DC_FAMILY_UWATEC_SMART},
	{"sensus",      DC_FAMILY_REEFNET_SENSUS},
	{"sensuspro",   DC_FAMILY_REEFNET_SENSUSPRO},
	{"sensusultra", DC_FAMILY_REEFNET_SENSUSULTRA},
	{"vtpro",       DC_FAMILY_OCEANIC_VTPRO},
	{"veo250",      DC_FAMILY_OCEANIC_VEO250},
	{"atom2",       DC_FAMILY_OCEANIC_ATOM2},
	{"nemo",        DC_FAMILY_MARES_NEMO},
	{"puck",        DC_FAMILY_MARES_PUCK},
	{"darwin",      DC_FAMILY_MARES_DARWIN},
	{"iconhd",      DC_FAMILY_MARES_ICONHD},
	{"ostc",        DC_FAMILY_HW_OSTC},
	{"frog",        DC_FAMILY_HW_FROG},
	{"edy",         DC_FAMILY_CRESSI_EDY},
	{"n2ition3",    DC_FAMILY_ZEAGLE_N2ITION3},
	{"cobalt",      DC_FAMILY_ATOMICS_COBALT},
	{"predator",	DC_FAMILY_SHEARWATER_PREDATOR}
};

static dc_family_t
lookup_type (const char *name)
{
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		if (strcmp (name, g_backends[i].name) == 0)
			return g_backends[i].type;
	}

	return DC_FAMILY_NULL;
}

static const char *
lookup_name (dc_family_t type)
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
fpread (const char *dirname, dc_family_t backend, unsigned int serial)
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
fpwrite (dc_buffer_t *buffer, const char *dirname, dc_family_t backend, unsigned int serial)
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
sample_cb (dc_sample_type_t type, dc_sample_value_t value, void *userdata)
{
	static const char *events[] = {
		"none", "deco", "rbt", "ascent", "ceiling", "workload", "transmitter",
		"violation", "bookmark", "surface", "safety stop", "gaschange",
		"safety stop (voluntary)", "safety stop (mandatory)", "deepstop",
		"ceiling (safety stop)", "unknown", "divetime", "maxdepth",
		"OLF", "PO2", "airtime", "rgbm", "heading", "tissue level warning",
		"gaschange2"};
	static const char *decostop[] = {
		"ndl", "deco", "deep", "safety"};

	sample_data_t *sampledata = (sample_data_t *) userdata;

	switch (type) {
	case DC_SAMPLE_TIME:
		if (sampledata->nsamples++)
			fprintf (sampledata->fp, "</sample>\n");
		fprintf (sampledata->fp, "<sample>\n");
		fprintf (sampledata->fp, "   <time>%02u:%02u</time>\n", value.time / 60, value.time % 60);
		break;
	case DC_SAMPLE_DEPTH:
		fprintf (sampledata->fp, "   <depth>%.2f</depth>\n", value.depth);
		break;
	case DC_SAMPLE_PRESSURE:
		fprintf (sampledata->fp, "   <pressure tank=\"%u\">%.2f</pressure>\n", value.pressure.tank, value.pressure.value);
		break;
	case DC_SAMPLE_TEMPERATURE:
		fprintf (sampledata->fp, "   <temperature>%.2f</temperature>\n", value.temperature);
		break;
	case DC_SAMPLE_EVENT:
		fprintf (sampledata->fp, "   <event type=\"%u\" time=\"%u\" flags=\"%u\" value=\"%u\">%s</event>\n",
			value.event.type, value.event.time, value.event.flags, value.event.value, events[value.event.type]);
		break;
	case DC_SAMPLE_RBT:
		fprintf (sampledata->fp, "   <rbt>%u</rbt>\n", value.rbt);
		break;
	case DC_SAMPLE_HEARTBEAT:
		fprintf (sampledata->fp, "   <heartbeat>%u</heartbeat>\n", value.heartbeat);
		break;
	case DC_SAMPLE_BEARING:
		fprintf (sampledata->fp, "   <bearing>%u</bearing>\n", value.bearing);
		break;
	case DC_SAMPLE_VENDOR:
		fprintf (sampledata->fp, "   <vendor type=\"%u\" size=\"%u\">", value.vendor.type, value.vendor.size);
		for (unsigned int i = 0; i < value.vendor.size; ++i)
			fprintf (sampledata->fp, "%02X", ((unsigned char *) value.vendor.data)[i]);
		fprintf (sampledata->fp, "</vendor>\n");
		break;
	case DC_SAMPLE_SETPOINT:
		fprintf (sampledata->fp, "   <setpoint>%.2f</setpoint>\n", value.setpoint);
		break;
	case DC_SAMPLE_PPO2:
		fprintf (sampledata->fp, "   <ppo2>%.2f</ppo2>\n", value.ppo2);
		break;
	case DC_SAMPLE_CNS:
		fprintf (sampledata->fp, "   <cns>%.2f</cns>\n", value.cns);
		break;
	case DC_SAMPLE_DECO:
		fprintf (sampledata->fp, "   <deco time=\"%u\" depth=\"%.2f\">%s</deco>\n",
			value.deco.time, value.deco.depth, decostop[value.deco.type]);
		break;
	default:
		break;
	}
}


static dc_status_t
doparse (FILE *fp, dc_device_t *device, const unsigned char data[], unsigned int size)
{
	// Create the parser.
	message ("Creating the parser.\n");
	dc_parser_t *parser = NULL;
	dc_status_t rc = dc_parser_new (&parser, device);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error creating the parser.");
		return rc;
	}

	// Register the data.
	message ("Registering the data.\n");
	rc = dc_parser_set_data (parser, data, size);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error registering the data.");
		dc_parser_destroy (parser);
		return rc;
	}

	// Parse the datetime.
	message ("Parsing the datetime.\n");
	dc_datetime_t dt = {0};
	rc = dc_parser_get_datetime (parser, &dt);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the datetime.");
		dc_parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<datetime>%04i-%02i-%02i %02i:%02i:%02i</datetime>\n",
		dt.year, dt.month, dt.day,
		dt.hour, dt.minute, dt.second);

	// Parse the divetime.
	message ("Parsing the divetime.\n");
	unsigned int divetime = 0;
	rc = dc_parser_get_field (parser, DC_FIELD_DIVETIME, 0, &divetime);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the divetime.");
		dc_parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<divetime>%02u:%02u</divetime>\n",
		divetime / 60, divetime % 60);

	// Parse the maxdepth.
	message ("Parsing the maxdepth.\n");
	double maxdepth = 0.0;
	rc = dc_parser_get_field (parser, DC_FIELD_MAXDEPTH, 0, &maxdepth);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the maxdepth.");
		dc_parser_destroy (parser);
		return rc;
	}

	fprintf (fp, "<maxdepth>%.2f</maxdepth>\n",
		maxdepth);

	// Parse the gas mixes.
	message ("Parsing the gas mixes.\n");
	unsigned int ngases = 0;
	rc = dc_parser_get_field (parser, DC_FIELD_GASMIX_COUNT, 0, &ngases);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the gas mix count.");
		dc_parser_destroy (parser);
		return rc;
	}

	for (unsigned int i = 0; i < ngases; ++i) {
		dc_gasmix_t gasmix = {0};
		rc = dc_parser_get_field (parser, DC_FIELD_GASMIX, i, &gasmix);
		if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
			WARNING ("Error parsing the gas mix.");
			dc_parser_destroy (parser);
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

	// Parse the salinity.
	message ("Parsing the salinity.\n");
	dc_salinity_t salinity = {DC_WATER_FRESH, 0.0};
	rc = dc_parser_get_field (parser, DC_FIELD_SALINITY, 0, &salinity);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the salinity.");
		dc_parser_destroy (parser);
		return rc;
	}

	if (rc != DC_STATUS_UNSUPPORTED) {
		fprintf (fp, "<salinity type=\"%u\">%.1f</salinity>\n",
			salinity.type, salinity.density);
	}

	// Parse the atmospheric pressure.
	message ("Parsing the atmospheric pressure.\n");
	double atmospheric = 0.0;
	rc = dc_parser_get_field (parser, DC_FIELD_ATMOSPHERIC, 0, &atmospheric);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		WARNING ("Error parsing the atmospheric pressure.");
		dc_parser_destroy (parser);
		return rc;
	}

	if (rc != DC_STATUS_UNSUPPORTED) {
		fprintf (fp, "<atmospheric>%.5f</atmospheric>\n",
			atmospheric);
	}

	// Initialize the sample data.
	sample_data_t sampledata = {0};
	sampledata.nsamples = 0;
	sampledata.fp = fp;

	// Parse the sample data.
	message ("Parsing the sample data.\n");
	rc = dc_parser_samples_foreach (parser, sample_cb, &sampledata);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error parsing the sample data.");
		dc_parser_destroy (parser);
		return rc;
	}

	if (sampledata.nsamples)
		fprintf (fp, "</sample>\n");

	// Destroy the parser.
	message ("Destroying the parser.\n");
	rc = dc_parser_destroy (parser);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error destroying the parser.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}

static void
event_cb (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata)
{
	const dc_event_progress_t *progress = (dc_event_progress_t *) data;
	const dc_event_devinfo_t *devinfo = (dc_event_devinfo_t *) data;
	const dc_event_clock_t *clock = (dc_event_clock_t *) data;
	const dc_event_vendor_t *vendor = (dc_event_vendor_t *) data;

	device_data_t *devdata = (device_data_t *) userdata;

	switch (event) {
	case DC_EVENT_WAITING:
		message ("Event: waiting for user action\n");
		break;
	case DC_EVENT_PROGRESS:
		message ("Event: progress %3.2f%% (%u/%u)\n",
			100.0 * (double) progress->current / (double) progress->maximum,
			progress->current, progress->maximum);
		break;
	case DC_EVENT_DEVINFO:
		devdata->devinfo = *devinfo;
		message ("Event: model=%u (0x%08x), firmware=%u (0x%08x), serial=%u (0x%08x)\n",
			devinfo->model, devinfo->model,
			devinfo->firmware, devinfo->firmware,
			devinfo->serial, devinfo->serial);
		if (g_cachedir && g_cachedir_read) {
			dc_buffer_t *fingerprint = fpread (g_cachedir, dc_device_get_type (device), devinfo->serial);
			dc_device_set_fingerprint (device,
				dc_buffer_get_data (fingerprint),
				dc_buffer_get_size (fingerprint));
			dc_buffer_free (fingerprint);
		}
		break;
	case DC_EVENT_CLOCK:
		devdata->clock = *clock;
		message ("Event: systime=" DC_TICKS_FORMAT ", devtime=%u\n",
			clock->systime, clock->devtime);
		break;
	case DC_EVENT_VENDOR:
		message ("Event: vendor=");
		for (unsigned int i = 0; i < vendor->size; ++i)
			message ("%02X", vendor->data[i]);
		message ("\n");
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

		doparse (divedata->fp, divedata->device, data, size);

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
	fprintf (stderr, "   -n name        Set device name (required).\n");
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

	fprintf (stderr, "Supported devices:\n\n");
	dc_iterator_t *iterator = NULL;
	dc_descriptor_t *descriptor = NULL;
	dc_descriptor_iterator (&iterator);
	while (dc_iterator_next (iterator, &descriptor) == DC_STATUS_SUCCESS) {
		fprintf (stderr, "   %s %s\n",
			dc_descriptor_get_vendor (descriptor),
			dc_descriptor_get_product (descriptor));
		dc_descriptor_free (descriptor);
	}
	dc_iterator_free (iterator);
}


static dc_status_t
search (dc_descriptor_t **out, const char *name, dc_family_t backend, unsigned int model)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	dc_iterator_t *iterator = NULL;
	rc = dc_descriptor_iterator (&iterator);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error creating the device descriptor iterator.");
		return rc;
	}

	dc_descriptor_t *descriptor = NULL, *current = NULL;
	while ((rc = dc_iterator_next (iterator, &descriptor)) == DC_STATUS_SUCCESS) {
		if (name) {
			const char *vendor = dc_descriptor_get_vendor (descriptor);
			const char *product = dc_descriptor_get_product (descriptor);

			size_t n = strlen (vendor);
			if (strncasecmp (name, vendor, n) == 0 && name[n] == ' ' &&
				strcasecmp (name + n + 1, product) == 0)
			{
				current = descriptor;
				break;
			} else if (strcasecmp (name, product) == 0) {
				current = descriptor;
				break;
			}
		} else {
			if (backend == dc_descriptor_get_type (descriptor)) {
				if (model == dc_descriptor_get_model (descriptor)) {
					// Exact match found. Return immediately.
					dc_descriptor_free (current);
					current = descriptor;
					break;
				} else {
					// Possible match found. Keep searching for an exact match.
					// If no exact match is found, the first match is returned.
					if (current == NULL) {
						current = descriptor;
						descriptor = NULL;
					}
				}
			}
		}

		dc_descriptor_free (descriptor);
	}

	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_DONE) {
		dc_descriptor_free (current);
		dc_iterator_free (iterator);
		WARNING ("Error iterating the device descriptors.");
		return rc;
	}

	dc_iterator_free (iterator);

	*out = current;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
dowork (dc_context_t *context, dc_descriptor_t *descriptor, const char *devname, const char *rawfile, const char *xmlfile, int memory, int dives, dc_buffer_t *fingerprint)
{
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Initialize the device data.
	device_data_t devdata = {{0}};

	// Open the device.
	message ("Opening the device (%s %s, %s).\n",
		dc_descriptor_get_vendor (descriptor),
		dc_descriptor_get_product (descriptor),
		devname ? devname : "null");
	dc_device_t *device = NULL;
	rc = dc_device_open (&device, context, descriptor, devname);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error opening device.");
		return rc;
	}

	// Register the event handler.
	message ("Registering the event handler.\n");
	int events = DC_EVENT_WAITING | DC_EVENT_PROGRESS | DC_EVENT_DEVINFO | DC_EVENT_CLOCK | DC_EVENT_VENDOR;
	rc = dc_device_set_events (device, events, event_cb, &devdata);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error registering the event handler.");
		dc_device_close (device);
		return rc;
	}

	// Register the cancellation handler.
	message ("Registering the cancellation handler.\n");
	rc = dc_device_set_cancel (device, cancel_cb, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error registering the cancellation handler.");
		dc_device_close (device);
		return rc;
	}

	// Register the fingerprint data.
	if (fingerprint) {
		message ("Registering the fingerprint data.\n");
		rc = dc_device_set_fingerprint (device, dc_buffer_get_data (fingerprint), dc_buffer_get_size (fingerprint));
		if (rc != DC_STATUS_SUCCESS) {
			WARNING ("Error registering the fingerprint data.");
			dc_device_close (device);
			return rc;
		}
	}

	if (memory) {
		// Allocate a memory buffer.
		dc_buffer_t *buffer = dc_buffer_new (0);

		// Download the memory dump.
		message ("Downloading the memory dump.\n");
		rc = dc_device_dump (device, buffer);
		if (rc != DC_STATUS_SUCCESS) {
			WARNING ("Error downloading the memory dump.");
			dc_buffer_free (buffer);
			dc_device_close (device);
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
		divedata.device = device;
		divedata.fingerprint = NULL;
		divedata.number = 0;

		// Open the output file.
		divedata.fp = fopen (xmlfile, "w");

		// Download the dives.
		message ("Downloading the dives.\n");
		rc = dc_device_foreach (device, dive_cb, &divedata);
		if (rc != DC_STATUS_SUCCESS) {
			WARNING ("Error downloading the dives.");
			dc_buffer_free (divedata.fingerprint);
			if (divedata.fp) fclose (divedata.fp);
			dc_device_close (device);
			return rc;
		}

		// Store the fingerprint data.
		if (g_cachedir) {
			fpwrite (divedata.fingerprint, g_cachedir, dc_device_get_type (device), devdata.devinfo.serial);
		}

		// Free the fingerprint buffer.
		dc_buffer_free (divedata.fingerprint);

		// Close the output file.
		if (divedata.fp) fclose (divedata.fp);
	}

	// Close the device.
	message ("Closing the device.\n");
	rc = dc_device_close (device);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error closing the device.");
		return rc;
	}

	return DC_STATUS_SUCCESS;
}


int
main (int argc, char *argv[])
{
	// Default values.
	dc_family_t backend = DC_FAMILY_NULL;
	dc_loglevel_t loglevel = DC_LOGLEVEL_WARNING;
	const char *name = NULL;
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
	while ((opt = getopt (argc, argv, "n:b:t:f:l:m:d:c:vh")) != -1) {
		switch (opt) {
		case 'n':
			name = optarg;
			break;
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
		case 'v':
			loglevel++;
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

	signal (SIGINT, sighandler);

	message_set_logfile (logfile);

	dc_context_t *context = NULL;
	dc_status_t rc = dc_context_new (&context);
	if (rc != DC_STATUS_SUCCESS) {
		message_set_logfile (NULL);
		return EXIT_FAILURE;
	}

	dc_context_set_loglevel (context, loglevel);
	dc_context_set_logfunc (context, logfunc, NULL);

	/* Search for a matching device descriptor. */
	dc_descriptor_t *descriptor = NULL;
	rc = search (&descriptor, name, backend, model);
	if (rc != DC_STATUS_SUCCESS) {
		message_set_logfile (NULL);
		return EXIT_FAILURE;
	}

	/* Fail if no device descriptor found. */
	if (descriptor == NULL) {
		WARNING ("No matching device found.");
		usage (argv[0]);
		message_set_logfile (NULL);
		return EXIT_FAILURE;
	}

	dc_buffer_t *fp = fpconvert (fingerprint);
	rc = dowork (context, descriptor, devname, rawfile, xmlfile, memory, dives, fp);
	dc_buffer_free (fp);
	message ("Result: %s\n", errmsg (rc));

	dc_descriptor_free (descriptor);
	dc_context_free (context);

	message_set_logfile (NULL);

	return rc != DC_STATUS_SUCCESS ? EXIT_FAILURE : EXIT_SUCCESS;
}
