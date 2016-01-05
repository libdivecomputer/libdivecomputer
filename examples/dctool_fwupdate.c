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
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/hw_ostc.h>
#include <libdivecomputer/hw_ostc3.h>

#include "dctool.h"
#include "common.h"
#include "utils.h"

static dc_status_t
fwupdate (dc_context_t *context, dc_descriptor_t *descriptor, const char *devname, const char *hexfile)
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
	int events = DC_EVENT_PROGRESS;
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

	// Update the firmware.
	message ("Updating the firmware.\n");
	switch (dc_device_get_type (device)) {
	case DC_FAMILY_HW_OSTC:
		rc = hw_ostc_device_fwupdate (device, hexfile);
		break;
	case DC_FAMILY_HW_OSTC3:
		rc = hw_ostc3_device_fwupdate (device, hexfile);
		break;
	default:
		rc = DC_STATUS_UNSUPPORTED;
		break;
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error updating the firmware.");
		goto cleanup;
	}

cleanup:
	dc_device_close (device);
	return rc;
}

static int
dctool_fwupdate_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;

	// Default option values.
	unsigned int help = 0;
	const char *filename = NULL;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "hf:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"firmware",    required_argument, 0, 'f'},
		{0,             0,                 0,  0 }
	};
	while ((opt = getopt_long (argc, argv, optstring, options, NULL)) != -1) {
#else
	while ((opt = getopt (argc, argv, optstring)) != -1) {
#endif
		switch (opt) {
		case 'f':
			filename = optarg;
			break;
		case 'h':
			help = 1;
			break;
		default:
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	// Show help message.
	if (help) {
		dctool_command_showhelp (&dctool_fwupdate);
		return EXIT_SUCCESS;
	}

	// Check mandatory arguments.
	if (!filename) {
		message ("No firmware file specified.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Update the firmware.
	status = fwupdate (context, descriptor, argv[0], filename);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	return exitcode;
}

const dctool_command_t dctool_fwupdate = {
	dctool_fwupdate_run,
	DCTOOL_CONFIG_DESCRIPTOR,
	"fwupdate",
	"Update the firmware",
	"Usage:\n"
	"   dctool fwupdate [options]\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help                  Show help message\n"
	"   -f, --firmware <filename>   Firmware filename\n"
#else
	"   -h              Show help message\n"
	"   -f <filename>   Firmware filename\n"
#endif
};
