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

#include "dctool.h"
#include "common.h"
#include "utils.h"

static dc_status_t
dump (dc_context_t *context, dc_descriptor_t *descriptor, const char *devname, dc_buffer_t *fingerprint, dc_buffer_t *buffer)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *device = NULL;

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

	// Register the event handler.
	message ("Registering the event handler.\n");
	int events = DC_EVENT_WAITING | DC_EVENT_PROGRESS | DC_EVENT_DEVINFO | DC_EVENT_CLOCK | DC_EVENT_VENDOR;
	rc = dc_device_set_events (device, events, dctool_event_cb, NULL);
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

	// Download the memory dump.
	message ("Downloading the memory dump.\n");
	rc = dc_device_dump (device, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error downloading the memory dump.");
		goto cleanup;
	}

cleanup:
	dc_device_close (device);
	return rc;
}

static int
dctool_dump_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *fingerprint = NULL;
	dc_buffer_t *buffer = NULL;

	// Default option values.
	unsigned int help = 0;
	const char *fphex = NULL;
	const char *filename = NULL;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "ho:p:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"output",      required_argument, 0, 'o'},
		{"fingerprint", required_argument, 0, 'p'},
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
		default:
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	// Show help message.
	if (help) {
		dctool_command_showhelp (&dctool_dump);
		return EXIT_SUCCESS;
	}

	// Convert the fingerprint to binary.
	fingerprint = dctool_convert_hex2bin (fphex);

	// Allocate a memory buffer.
	buffer = dc_buffer_new (0);

	// Download the memory dump.
	status = dump (context, descriptor, argv[0], fingerprint, buffer);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Write the memory dump to disk.
	dctool_file_write (filename, buffer);

cleanup:
	dc_buffer_free (buffer);
	dc_buffer_free (fingerprint);
	return exitcode;
}

const dctool_command_t dctool_dump = {
	dctool_dump_run,
	DCTOOL_CONFIG_DESCRIPTOR,
	"dump",
	"Download a memory dump",
	"Usage:\n"
	"   dctool dump [options] <devname>\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help                 Show help message\n"
	"   -o, --output <filename>    Output filename\n"
	"   -p, --fingerprint <data>   Fingerprint data (hexadecimal)\n"
#else
	"   -h                 Show help message\n"
	"   -o <filename>      Output filename\n"
	"   -p <fingerprint>   Fingerprint data (hexadecimal)\n"
#endif
};
