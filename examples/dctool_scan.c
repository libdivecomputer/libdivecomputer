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
#include <unistd.h>
#include <stdio.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/iterator.h>
#include <libdivecomputer/serial.h>
#include <libdivecomputer/irda.h>
#include <libdivecomputer/bluetooth.h>
#include <libdivecomputer/usb.h>
#include <libdivecomputer/usbhid.h>

#include "dctool.h"
#include "common.h"
#include "utils.h"

static dc_status_t
scan (dc_context_t *context, dc_descriptor_t *descriptor, dc_transport_t transport)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_iterator_t *iterator = NULL;

	// Create the device iterator.
	switch (transport) {
	case DC_TRANSPORT_SERIAL:
		status = dc_serial_iterator_new (&iterator, context, descriptor);
		break;
	case DC_TRANSPORT_IRDA:
		status = dc_irda_iterator_new (&iterator, context, descriptor);
		break;
	case DC_TRANSPORT_BLUETOOTH:
		status = dc_bluetooth_iterator_new (&iterator, context, descriptor);
		break;
	case DC_TRANSPORT_USB:
		status = dc_usb_iterator_new (&iterator, context, descriptor);
		break;
	case DC_TRANSPORT_USBHID:
		status = dc_usbhid_iterator_new (&iterator, context, descriptor);
		break;
	default:
		status = DC_STATUS_UNSUPPORTED;
		break;
	}
	if (status != DC_STATUS_SUCCESS) {
		ERROR ("Failed to create the device iterator.");
		goto cleanup;
	}

	// Enumerate the devices.
	void *device = NULL;
	while ((status = dc_iterator_next (iterator, &device)) == DC_STATUS_SUCCESS) {
		char buffer[DC_BLUETOOTH_SIZE];
		switch (transport) {
		case DC_TRANSPORT_SERIAL:
			printf ("%s\n", dc_serial_device_get_name (device));
			dc_serial_device_free (device);
			break;
		case DC_TRANSPORT_IRDA:
			printf ("%08x\t%s\n", dc_irda_device_get_address (device), dc_irda_device_get_name (device));
			dc_irda_device_free (device);
			break;
		case DC_TRANSPORT_BLUETOOTH:
			printf ("%s\t%s\n",
				dc_bluetooth_addr2str(dc_bluetooth_device_get_address (device), buffer, sizeof(buffer)),
				dc_bluetooth_device_get_name (device));
			dc_bluetooth_device_free (device);
			break;
		case DC_TRANSPORT_USB:
			printf ("%04x:%04x\n", dc_usb_device_get_vid (device), dc_usb_device_get_pid (device));
			dc_usb_device_free (device);
			break;
		case DC_TRANSPORT_USBHID:
			printf ("%04x:%04x\n", dc_usbhid_device_get_vid (device), dc_usbhid_device_get_pid (device));
			dc_usbhid_device_free (device);
			break;
		default:
			break;
		}
	}
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_DONE) {
		ERROR ("Failed to enumerate the devices.");
		goto cleanup;
	}

	status = DC_STATUS_SUCCESS;

cleanup:
	dc_iterator_free (iterator);
	return status;
}

static int
dctool_scan_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;

	// Default option values.
	unsigned int help = 0;
	dc_transport_t transport = dctool_transport_default (descriptor);

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
		dctool_command_showhelp (&dctool_scan);
		return EXIT_SUCCESS;
	}

	// Check the transport type.
	if (transport == DC_TRANSPORT_NONE) {
		message ("No valid transport type specified.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Scan for supported devices.
	status = scan (context, descriptor, transport);
	if (status != DC_STATUS_SUCCESS) {
		message ("ERROR: %s\n", dctool_errmsg (status));
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

cleanup:
	return exitcode;
}

const dctool_command_t dctool_scan = {
	dctool_scan_run,
	DCTOOL_CONFIG_NONE,
	"scan",
	"Scan for supported devices",
	"Usage:\n"
	"   dctool scan [options]\n"
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
