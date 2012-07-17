/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#include <libdivecomputer/hw_frog.h>

#include "utils.h"
#include "common.h"

dc_status_t
test_dump_memory (const char* name, const char* filename)
{
	dc_context_t *context = NULL;
	dc_device_t *device = NULL;

	dc_context_new (&context);
	dc_context_set_loglevel (context, DC_LOGLEVEL_ALL);
	dc_context_set_logfunc (context, logfunc, NULL);

	message ("hw_frog_device_open\n");
	dc_status_t rc = hw_frog_device_open (&device, context, name);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		dc_context_free (context);
		return rc;
	}

	message ("dc_device_foreach\n");
	rc = dc_device_foreach (device, NULL, NULL);
	if (rc != DC_STATUS_SUCCESS) {
		WARNING ("Cannot read memory.");
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
	message_set_logfile ("FROG.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	dc_status_t a = test_dump_memory (name, "FROG.DMP");

	message ("SUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory:          %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
