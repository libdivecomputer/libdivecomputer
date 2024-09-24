/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
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
do_timesync (dc_context_t *context, dc_descriptor_t *descriptor, dc_transport_t transport, const char *devname, const dc_datetime_t *datetime)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_iostream_t *iostream = NULL;
	dc_device_t *device = NULL;

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

	// Syncronize the device clock.
	message ("Synchronize the device clock.\n");
	rc = dc_device_timesync (device, datetime);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error synchronizing the device clock.");
		goto cleanup;
	}

cleanup:
	dc_device_close (device);
	dc_iostream_close (iostream);
	return rc;
}

static int
dctool_timesync_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_transport_t transport = dctool_transport_default (descriptor);

	// Default option values.
	unsigned int help = 0;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "ht:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"transport",   required_argument, 0, 't'},
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
		default:
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	// Show help message.
	if (help) {
		dctool_command_showhelp (&dctool_timesync);
		return EXIT_SUCCESS;
	}

	// Check the transport type.
	if (transport == DC_TRANSPORT_NONE) {
		message ("No valid transport type specified.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Get the system time.
	dc_datetime_t datetime = {0};
	dc_ticks_t now = dc_datetime_now ();
	if (!dc_datetime_localtime(&datetime, now)) {
		message ("ERROR: Failed to get the system time.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Synchronize the device clock.
	status = do_timesync (context, descriptor, transport, argv[0], &datetime);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	return exitcode;
}

const dctool_command_t dctool_timesync = {
	dctool_timesync_run,
	DCTOOL_CONFIG_DESCRIPTOR,
	"timesync",
	"Synchronize the device clock",
	"Usage:\n"
	"   dctool timesync [options]\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help               Show help message\n"
	"   -t, --transport <name>   Transport type\n"
#else
	"   -h               Show help message\n"
	"   -t <transport>   Transport type\n"
#endif
};
