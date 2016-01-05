/*
 * libdivecomputer
 *
 * Copyright (C) 2015 Jef Driesen
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>

#include "dctool.h"
#include "common.h"
#include "utils.h"

typedef struct event_data_t {
	const char *cachedir;
	dc_event_devinfo_t devinfo;
} event_data_t;

typedef struct dive_data_t {
	FILE* ostream;
	dc_device_t *device;
	dc_buffer_t **fingerprint;
	unsigned int number;
} dive_data_t;

typedef struct sample_data_t {
	FILE* ostream;
	unsigned int nsamples;
} sample_data_t;

static void
sample_cb (dc_sample_type_t type, dc_sample_value_t value, void *userdata)
{
	static const char *events[] = {
		"none", "deco", "rbt", "ascent", "ceiling", "workload", "transmitter",
		"violation", "bookmark", "surface", "safety stop", "gaschange",
		"safety stop (voluntary)", "safety stop (mandatory)", "deepstop",
		"ceiling (safety stop)", "floor", "divetime", "maxdepth",
		"OLF", "PO2", "airtime", "rgbm", "heading", "tissue level warning",
		"gaschange2"};
	static const char *decostop[] = {
		"ndl", "safety", "deco", "deep"};

	sample_data_t *sampledata = (sample_data_t *) userdata;

	switch (type) {
	case DC_SAMPLE_TIME:
		if (sampledata->nsamples++)
			fprintf (sampledata->ostream, "</sample>\n");
		fprintf (sampledata->ostream, "<sample>\n");
		fprintf (sampledata->ostream, "   <time>%02u:%02u</time>\n", value.time / 60, value.time % 60);
		break;
	case DC_SAMPLE_DEPTH:
		fprintf (sampledata->ostream, "   <depth>%.2f</depth>\n", value.depth);
		break;
	case DC_SAMPLE_PRESSURE:
		fprintf (sampledata->ostream, "   <pressure tank=\"%u\">%.2f</pressure>\n", value.pressure.tank, value.pressure.value);
		break;
	case DC_SAMPLE_TEMPERATURE:
		fprintf (sampledata->ostream, "   <temperature>%.2f</temperature>\n", value.temperature);
		break;
	case DC_SAMPLE_EVENT:
		if (value.event.type != SAMPLE_EVENT_GASCHANGE && value.event.type != SAMPLE_EVENT_GASCHANGE2) {
			fprintf (sampledata->ostream, "   <event type=\"%u\" time=\"%u\" flags=\"%u\" value=\"%u\">%s</event>\n",
				value.event.type, value.event.time, value.event.flags, value.event.value, events[value.event.type]);
		}
		break;
	case DC_SAMPLE_RBT:
		fprintf (sampledata->ostream, "   <rbt>%u</rbt>\n", value.rbt);
		break;
	case DC_SAMPLE_HEARTBEAT:
		fprintf (sampledata->ostream, "   <heartbeat>%u</heartbeat>\n", value.heartbeat);
		break;
	case DC_SAMPLE_BEARING:
		fprintf (sampledata->ostream, "   <bearing>%u</bearing>\n", value.bearing);
		break;
	case DC_SAMPLE_VENDOR:
		fprintf (sampledata->ostream, "   <vendor type=\"%u\" size=\"%u\">", value.vendor.type, value.vendor.size);
		for (unsigned int i = 0; i < value.vendor.size; ++i)
			fprintf (sampledata->ostream, "%02X", ((unsigned char *) value.vendor.data)[i]);
		fprintf (sampledata->ostream, "</vendor>\n");
		break;
	case DC_SAMPLE_SETPOINT:
		fprintf (sampledata->ostream, "   <setpoint>%.2f</setpoint>\n", value.setpoint);
		break;
	case DC_SAMPLE_PPO2:
		fprintf (sampledata->ostream, "   <ppo2>%.2f</ppo2>\n", value.ppo2);
		break;
	case DC_SAMPLE_CNS:
		fprintf (sampledata->ostream, "   <cns>%.1f</cns>\n", value.cns * 100.0);
		break;
	case DC_SAMPLE_DECO:
		fprintf (sampledata->ostream, "   <deco time=\"%u\" depth=\"%.2f\">%s</deco>\n",
			value.deco.time, value.deco.depth, decostop[value.deco.type]);
		break;
	case DC_SAMPLE_GASMIX:
		fprintf (sampledata->ostream, "   <gasmix>%u</gasmix>\n", value.gasmix);
		break;
	default:
		break;
	}
}

static dc_status_t
doparse (FILE *ostream, dc_device_t *device, const unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_parser_t *parser = NULL;

	// Create the parser.
	message ("Creating the parser.\n");
	rc = dc_parser_new (&parser, device);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error creating the parser.");
		goto cleanup;
	}

	// Register the data.
	message ("Registering the data.\n");
	rc = dc_parser_set_data (parser, data, size);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error registering the data.");
		goto cleanup;
	}

	// Parse the datetime.
	message ("Parsing the datetime.\n");
	dc_datetime_t dt = {0};
	rc = dc_parser_get_datetime (parser, &dt);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the datetime.");
		goto cleanup;
	}

	fprintf (ostream, "<datetime>%04i-%02i-%02i %02i:%02i:%02i</datetime>\n",
		dt.year, dt.month, dt.day,
		dt.hour, dt.minute, dt.second);

	// Parse the divetime.
	message ("Parsing the divetime.\n");
	unsigned int divetime = 0;
	rc = dc_parser_get_field (parser, DC_FIELD_DIVETIME, 0, &divetime);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the divetime.");
		goto cleanup;
	}

	fprintf (ostream, "<divetime>%02u:%02u</divetime>\n",
		divetime / 60, divetime % 60);

	// Parse the maxdepth.
	message ("Parsing the maxdepth.\n");
	double maxdepth = 0.0;
	rc = dc_parser_get_field (parser, DC_FIELD_MAXDEPTH, 0, &maxdepth);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the maxdepth.");
		goto cleanup;
	}

	fprintf (ostream, "<maxdepth>%.2f</maxdepth>\n",
		maxdepth);

	// Parse the temperature.
	message ("Parsing the temperature.\n");
	for (unsigned int i = 0; i < 3; ++i) {
		dc_field_type_t fields[] = {DC_FIELD_TEMPERATURE_SURFACE,
			DC_FIELD_TEMPERATURE_MINIMUM,
			DC_FIELD_TEMPERATURE_MAXIMUM};
		const char *names[] = {"surface", "minimum", "maximum"};

		double temperature = 0.0;
		rc = dc_parser_get_field (parser, fields[i], 0, &temperature);
		if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the temperature.");
			goto cleanup;
		}

		if (rc != DC_STATUS_UNSUPPORTED) {
			fprintf (ostream, "<temperature type=\"%s\">%.1f</temperature>\n",
				names[i], temperature);
		}
	}

	// Parse the gas mixes.
	message ("Parsing the gas mixes.\n");
	unsigned int ngases = 0;
	rc = dc_parser_get_field (parser, DC_FIELD_GASMIX_COUNT, 0, &ngases);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the gas mix count.");
		goto cleanup;
	}

	for (unsigned int i = 0; i < ngases; ++i) {
		dc_gasmix_t gasmix = {0};
		rc = dc_parser_get_field (parser, DC_FIELD_GASMIX, i, &gasmix);
		if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the gas mix.");
			goto cleanup;
		}

		fprintf (ostream,
			"<gasmix>\n"
			"   <he>%.1f</he>\n"
			"   <o2>%.1f</o2>\n"
			"   <n2>%.1f</n2>\n"
			"</gasmix>\n",
			gasmix.helium * 100.0,
			gasmix.oxygen * 100.0,
			gasmix.nitrogen * 100.0);
	}

	// Parse the tanks.
	message ("Parsing the tanks.\n");
	unsigned int ntanks = 0;
	rc = dc_parser_get_field (parser, DC_FIELD_TANK_COUNT, 0, &ntanks);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the tank count.");
		goto cleanup;
	}

	for (unsigned int i = 0; i < ntanks; ++i) {
		const char *names[] = {"none", "metric", "imperial"};

		dc_tank_t tank = {0};
		rc = dc_parser_get_field (parser, DC_FIELD_TANK, i, &tank);
		if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the tank.");
			goto cleanup;
		}

		fprintf (ostream, "<tank>\n");
		if (tank.gasmix != DC_GASMIX_UNKNOWN) {
			fprintf (ostream,
				"   <gasmix>%u</gasmix>\n",
				tank.gasmix);
		}
		if (tank.type != DC_TANKVOLUME_NONE) {
			fprintf (ostream,
				"   <type>%s</type>\n"
				"   <volume>%.1f</volume>\n"
				"   <workpressure>%.2f</workpressure>\n",
				names[tank.type], tank.volume, tank.workpressure);
		}
		fprintf (ostream,
			"   <beginpressure>%.2f</beginpressure>\n"
			"   <endpressure>%.2f</endpressure>\n"
			"</tank>\n",
			tank.beginpressure, tank.endpressure);
	}

	// Parse the dive mode.
	message ("Parsing the dive mode.\n");
	dc_divemode_t divemode = DC_DIVEMODE_OC;
	rc = dc_parser_get_field (parser, DC_FIELD_DIVEMODE, 0, &divemode);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the dive mode.");
		goto cleanup;
	}

	if (rc != DC_STATUS_UNSUPPORTED) {
		const char *names[] = {"freedive", "gauge", "oc", "cc"};
		fprintf (ostream, "<divemode>%s</divemode>\n",
			names[divemode]);
	}

	// Parse the salinity.
	message ("Parsing the salinity.\n");
	dc_salinity_t salinity = {DC_WATER_FRESH, 0.0};
	rc = dc_parser_get_field (parser, DC_FIELD_SALINITY, 0, &salinity);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the salinity.");
		goto cleanup;
	}

	if (rc != DC_STATUS_UNSUPPORTED) {
		fprintf (ostream, "<salinity type=\"%u\">%.1f</salinity>\n",
			salinity.type, salinity.density);
	}

	// Parse the atmospheric pressure.
	message ("Parsing the atmospheric pressure.\n");
	double atmospheric = 0.0;
	rc = dc_parser_get_field (parser, DC_FIELD_ATMOSPHERIC, 0, &atmospheric);
	if (rc != DC_STATUS_SUCCESS && rc != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the atmospheric pressure.");
		goto cleanup;
	}

	if (rc != DC_STATUS_UNSUPPORTED) {
		fprintf (ostream, "<atmospheric>%.5f</atmospheric>\n",
			atmospheric);
	}

	// Initialize the sample data.
	sample_data_t sampledata = {0};
	sampledata.nsamples = 0;
	sampledata.ostream = ostream;

	// Parse the sample data.
	message ("Parsing the sample data.\n");
	rc = dc_parser_samples_foreach (parser, sample_cb, &sampledata);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error parsing the sample data.");
		goto cleanup;
	}

	if (sampledata.nsamples)
		fprintf (ostream, "</sample>\n");

cleanup:
	dc_parser_destroy (parser);
	return rc;
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

	// Keep a copy of the most recent fingerprint. Because dives are
	// guaranteed to be downloaded in reverse order, the most recent
	// dive is always the first dive.
	if (divedata->number == 1) {
		dc_buffer_t *fp = dc_buffer_new (fsize);
		dc_buffer_append (fp, fingerprint, fsize);
		*divedata->fingerprint = fp;
	}

	fprintf (divedata->ostream, "<dive>\n<number>%u</number>\n<size>%u</size>\n<fingerprint>", divedata->number, size);
	for (unsigned int i = 0; i < fsize; ++i)
		fprintf (divedata->ostream, "%02X", fingerprint[i]);
	fprintf (divedata->ostream, "</fingerprint>\n");

	doparse (divedata->ostream, divedata->device, data, size);

	fprintf (divedata->ostream, "</dive>\n");

	return 1;
}

static void
event_cb (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata)
{
	const dc_event_devinfo_t *devinfo = (const dc_event_devinfo_t *) data;

	event_data_t *eventdata = (event_data_t *) userdata;

	// Forward to the default event handler.
	dctool_event_cb (device, event, data, userdata);

	switch (event) {
	case DC_EVENT_DEVINFO:
		// Load the fingerprint from the cache. If there is no
		// fingerprint present in the cache, a NULL buffer is returned,
		// and the registered fingerprint will be cleared.
		if (eventdata->cachedir) {
			char filename[1024] = {0};
			dc_family_t family = DC_FAMILY_NULL;
			dc_buffer_t *fingerprint = NULL;

			// Generate the fingerprint filename.
			family = dc_device_get_type (device);
			snprintf (filename, sizeof (filename), "%s/%s-%08X.bin",
				eventdata->cachedir, dctool_family_name (family), devinfo->serial);

			// Read the fingerprint file.
			fingerprint = dctool_file_read (filename);

			// Register the fingerprint data.
			dc_device_set_fingerprint (device,
				dc_buffer_get_data (fingerprint),
				dc_buffer_get_size (fingerprint));

			// Free the buffer again.
			dc_buffer_free (fingerprint);
		}

		// Keep a copy of the event data. It will be used for generating
		// the fingerprint filename again after a (successful) download.
		eventdata->devinfo = *devinfo;
		break;
	default:
		break;
	}
}

static dc_status_t
download (dc_context_t *context, dc_descriptor_t *descriptor, const char *devname, const char *cachedir, dc_buffer_t *fingerprint, FILE *ostream)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *device = NULL;
	dc_buffer_t *ofingerprint = NULL;

	// Open the device.
	message ("Opening the device (%s %s, %s).\n",
		dc_descriptor_get_vendor (descriptor),
		dc_descriptor_get_product (descriptor),
		devname ? devname : "null");
	rc = dc_device_open (&device, context, descriptor, devname);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error opening the device.");
		goto cleanup;
	}

	// Initialize the event data.
	event_data_t eventdata = {0};
	if (fingerprint) {
		eventdata.cachedir = NULL;
	} else {
		eventdata.cachedir = cachedir;
	}

	// Register the event handler.
	message ("Registering the event handler.\n");
	int events = DC_EVENT_WAITING | DC_EVENT_PROGRESS | DC_EVENT_DEVINFO | DC_EVENT_CLOCK | DC_EVENT_VENDOR;
	rc = dc_device_set_events (device, events, event_cb, &eventdata);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error registering the event handler.");
		goto cleanup;
	}

	// Register the cancellation handler.
	message ("Registering the cancellation handler.\n");
	rc = dc_device_set_cancel (device, dctool_cancel_cb, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error registering the cancellation handler.");
		goto cleanup;
	}

	// Register the fingerprint data.
	if (fingerprint) {
		message ("Registering the fingerprint data.\n");
		rc = dc_device_set_fingerprint (device, dc_buffer_get_data (fingerprint), dc_buffer_get_size (fingerprint));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR ("Error registering the fingerprint data.");
			goto cleanup;
		}
	}

	// Initialize the dive data.
	dive_data_t divedata = {0};
	divedata.device = device;
	divedata.ostream = ostream;
	divedata.fingerprint = &ofingerprint;
	divedata.number = 0;

	fprintf (ostream, "<device>\n");

	// Download the dives.
	message ("Downloading the dives.\n");
	rc = dc_device_foreach (device, dive_cb, &divedata);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error downloading the dives.");
		goto cleanup;
	}

	fprintf (ostream, "</device>\n");

	// Store the fingerprint data.
	if (cachedir && ofingerprint) {
		char filename[1024] = {0};
		dc_family_t family = DC_FAMILY_NULL;

		// Generate the fingerprint filename.
		family = dc_device_get_type (device);
		snprintf (filename, sizeof (filename), "%s/%s-%08X.bin",
			cachedir, dctool_family_name (family), eventdata.devinfo.serial);

		// Write the fingerprint file.
		dctool_file_write (filename, ofingerprint);
	}

cleanup:
	dc_buffer_free (ofingerprint);
	dc_device_close (device);
	return rc;
}

static int
dctool_download_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *fingerprint = NULL;
	FILE *ostream = NULL;

	// Default option values.
	unsigned int help = 0;
	const char *fphex = NULL;
	const char *filename = NULL;
	const char *cachedir = NULL;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "ho:p:c:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"output",      required_argument, 0, 'o'},
		{"fingerprint", required_argument, 0, 'p'},
		{"cache",       required_argument, 0, 'c'},
		{0,             0,                 0,  0 }
	};
	while ((opt = getopt_long (argc, argv, optstring, options, NULL)) != -1) {
#else
	while ((opt = getopt (argc, argv, optstring)) != -1) {
#endif
		switch (opt) {
		case 'h':
			help = 1;
			break;
		case 'o':
			filename = optarg;
			break;
		case 'p':
			fphex = optarg;
			break;
		case 'c':
			cachedir = optarg;
			break;
		default:
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	// Show help message.
	if (help) {
		dctool_command_showhelp (&dctool_download);
		return EXIT_SUCCESS;
	}

	// Convert the fingerprint to binary.
	fingerprint = dctool_convert_hex2bin (fphex);

	// Open the output file.
	ostream = fopen (filename, "w");
	if (ostream == NULL) {
		message ("Failed to open the output file.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Download the dives.
	status = download (context, descriptor, argv[0], cachedir, fingerprint, ostream);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	if (ostream) fclose (ostream);
	dc_buffer_free (fingerprint);
	return exitcode;
}

const dctool_command_t dctool_download = {
	dctool_download_run,
	DCTOOL_CONFIG_DESCRIPTOR,
	"download",
	"Download the dives",
	"Usage:\n"
	"   dctool download [options] <devname>\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help                 Show help message\n"
	"   -o, --output <filename>    Output filename\n"
	"   -p, --fingerprint <data>   Fingerprint data (hexadecimal)\n"
	"   -c, --cache <directory>    Cache directory\n"
#else
	"   -h                 Show help message\n"
	"   -o <filename>      Output filename\n"
	"   -p <fingerprint>   Fingerprint data (hexadecimal)\n"
	"   -c <directory>     Cache directory\n"
#endif
};
