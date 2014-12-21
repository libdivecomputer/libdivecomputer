/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include <string.h>

#include <libdivecomputer/hw_ostc.h>
#include <libdivecomputer/hw_ostc3.h>

#include "utils.h"
#include "common.h"

static void
event_cb (dc_device_t *device, dc_event_type_t event, const void *data, void *userdata)
{
	const dc_event_progress_t *progress = (dc_event_progress_t *) data;

	switch (event) {
	case DC_EVENT_PROGRESS:
		message ("Event: progress %3.2f%% (%u/%u)\n",
			100.0 * (double) progress->current / (double) progress->maximum,
			progress->current, progress->maximum);
		break;
	default:
		break;
	}
}

static dc_status_t
fwupdate (const char *name, const char *hexfile, int ostc3)
{
	dc_context_t *context = NULL;
	dc_device_t *device = NULL;
	dc_status_t rc = DC_STATUS_SUCCESS;

	dc_context_new (&context);
	dc_context_set_loglevel (context, DC_LOGLEVEL_ALL);
	dc_context_set_logfunc (context, logfunc, NULL);

	if (ostc3) {
		message ("hw_ostc3_device_open\n");
		rc = hw_ostc3_device_open (&device, context, name);
	} else {
		message ("hw_ostc_device_open\n");
		rc = hw_ostc_device_open (&device, context, name);
	}
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		dc_context_free (context);
		return rc;
	}

	message ("dc_device_set_events.\n");
	rc = dc_device_set_events (device, DC_EVENT_PROGRESS, event_cb, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error registering the event handler.");
		dc_device_close (device);
		dc_context_free (context);
		return rc;
	}

	if (ostc3) {
		message ("hw_ostc3_device_fwupdate\n");
		rc = hw_ostc3_device_fwupdate (device, hexfile);
	} else {
		message ("hw_ostc_device_fwupdate\n");
		rc = hw_ostc_device_fwupdate (device, hexfile);
	}
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error flashing firmware.");
		dc_device_close (device);
		dc_context_free (context);
		return rc;
	}

	message ("dc_device_close\n");
	rc = dc_device_close (device);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		dc_context_free (context);
		return rc;
	}

	dc_context_free (context);

	return DC_STATUS_SUCCESS;
}


int main(int argc, char *argv[])
{
	message_set_logfile ("OSTC-FWUPDATE.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyUSB0";
#endif
	const char *hexfile = NULL;
	int ostc3 = 0;

	if (argc > 1) {
		name = argv[1];
	}
	if (argc > 2) {
		hexfile = argv[2];
	}
	if (argc > 3) {
	   if (strcmp(argv[3], "-3") == 0) {
		   ostc3 = 1;
	   } else {
		   ostc3 = 0;
	   }
	}

	message ("DEVICE=%s\n", name);
	message ("HEXFILE=%s\n", hexfile);

	dc_status_t a = fwupdate (name, hexfile, ostc3);

	message ("SUMMARY\n");
	message ("-------\n");
	message ("fwupdate: %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
