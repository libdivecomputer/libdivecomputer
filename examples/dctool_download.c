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
#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>

#include "dctool.h"
#include "common.h"
#include "output.h"
#include "utils.h"

typedef struct event_data_t {
	const char *cachedir;
	dc_event_devinfo_t devinfo;
} event_data_t;

typedef struct dive_data_t {
	dc_device_t *device;
	dc_buffer_t **fingerprint;
	unsigned int number;
	dctool_output_t *output;
} dive_data_t;

static int
dive_cb (const unsigned char *data, unsigned int size, const unsigned char *fingerprint, unsigned int fsize, void *userdata)
{
	dive_data_t *divedata = (dive_data_t *) userdata;
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_parser_t *parser = NULL;

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

	// Create the parser.
	message ("Creating the parser.\n");
	rc = dc_parser_new (&parser, divedata->device, data, size);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error creating the parser.");
		goto cleanup;
	}

	// Parse the dive data.
	message ("Parsing the dive data.\n");
	rc = dctool_output_write (divedata->output, parser, data, size, fingerprint, fsize);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error parsing the dive data.");
		goto cleanup;
	}

cleanup:
	dc_parser_destroy (parser);
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
download (dc_context_t *context, dc_descriptor_t *descriptor, dc_transport_t transport, const char *devname, const char *cachedir, dc_buffer_t *fingerprint, dctool_output_t *output)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_iostream_t *iostream = NULL;
	dc_device_t *device = NULL;
	dc_buffer_t *ofingerprint = NULL;

	// Open the I/O stream.
	message ("Opening the I/O stream (%s, %s).\n",
		dctool_transport_name (transport),
		devname ? devname : "null");
	rc = dctool_iostream_open (&iostream, context, descriptor, transport, devname);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error opening the I/O stream.");
		goto cleanup;
	}

	// Open the device.
	message ("Opening the device (%s %s).\n",
		dc_descriptor_get_vendor (descriptor),
		dc_descriptor_get_product (descriptor));
	rc = dc_device_open (&device, context, descriptor, iostream);
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
	divedata.fingerprint = &ofingerprint;
	divedata.number = 0;
	divedata.output = output;

	// Download the dives.
	message ("Downloading the dives.\n");
	rc = dc_device_foreach (device, dive_cb, &divedata);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error downloading the dives.");
		goto cleanup;
	}

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
	dc_iostream_close (iostream);
	return rc;
}

static int
dctool_download_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *fingerprint = NULL;
	dctool_output_t *output = NULL;
	dctool_units_t units = DCTOOL_UNITS_METRIC;
	dc_transport_t transport = dctool_transport_default (descriptor);

	// Default option values.
	unsigned int help = 0;
	const char *fphex = NULL;
	const char *filename = NULL;
	const char *cachedir = NULL;
	const char *format = "xml";

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "ht:o:p:c:f:u:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"transport",   required_argument, 0, 't'},
		{"output",      required_argument, 0, 'o'},
		{"fingerprint", required_argument, 0, 'p'},
		{"cache",       required_argument, 0, 'c'},
		{"format",      required_argument, 0, 'f'},
		{"units",       required_argument, 0, 'u'},
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
		case 't':
			transport = dctool_transport_type (optarg);
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
		case 'f':
			format = optarg;
			break;
		case 'u':
			if (strcmp (optarg, "metric") == 0)
				units = DCTOOL_UNITS_METRIC;
			if (strcmp (optarg, "imperial") == 0)
				units = DCTOOL_UNITS_IMPERIAL;
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

	// Check the transport type.
	if (transport == DC_TRANSPORT_NONE) {
		message ("No valid transport type specified.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Convert the fingerprint to binary.
	fingerprint = dctool_convert_hex2bin (fphex);

	// Create the output.
	if (strcasecmp(format, "raw") == 0) {
		output = dctool_raw_output_new (filename);
	} else if (strcasecmp(format, "xml") == 0) {
		output = dctool_xml_output_new (filename, units);
	} else {
		message ("Unknown output format: %s\n", format);
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}
	if (output == NULL) {
		message ("Failed to create the output.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Download the dives.
	status = download (context, descriptor, transport, argv[0], cachedir, fingerprint, output);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	dctool_output_free (output);
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
	"   -t, --transport <name>     Transport type\n"
	"   -o, --output <filename>    Output filename\n"
	"   -p, --fingerprint <data>   Fingerprint data (hexadecimal)\n"
	"   -c, --cache <directory>    Cache directory\n"
	"   -f, --format <format>      Output format\n"
	"   -u, --units <units>        Set units (metric or imperial)\n"
#else
	"   -h                 Show help message\n"
	"   -t <transport>     Transport type\n"
	"   -o <filename>      Output filename\n"
	"   -p <fingerprint>   Fingerprint data (hexadecimal)\n"
	"   -c <directory>     Cache directory\n"
	"   -f <format>        Output format\n"
	"   -u <units>         Set units (metric or imperial)\n"
#endif
	"\n"
	"Supported output formats:\n"
	"\n"
	"   XML (default)\n"
	"\n"
	"      All dives are exported to a single xml file.\n"
	"\n"
	"   RAW\n"
	"\n"
	"      Each dive is exported to a raw (binary) file. To output multiple\n"
	"      files, the filename is interpreted as a template and should\n"
	"      contain one or more placeholders.\n"
	"\n"
	"Supported template placeholders:\n"
	"\n"
	"   %f   Fingerprint (hexadecimal format)\n"
	"   %n   Number (4 digits)\n"
	"   %t   Timestamp (basic ISO 8601 date/time format)\n"
};
