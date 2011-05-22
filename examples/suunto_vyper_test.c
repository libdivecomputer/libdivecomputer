/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
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
#include <stdlib.h>	// atoi

#include "suunto_vyper.h"
#include "utils.h"

#include "common.h"

device_status_t
test_dump_sdm (const char* name, unsigned int delay)
{
	device_t *device = NULL;

	message ("suunto_vyper_device_open\n");
	device_status_t rc = suunto_vyper_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	suunto_vyper_device_set_delay (device, delay);

	message ("device_foreach\n");
	rc = device_foreach (device, NULL, NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read dives.");
		device_close (device);
		return rc;
	}

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
test_dump_memory (const char* name, unsigned int delay, const char* filename)
{
	device_t *device = NULL;

	message ("suunto_vyper_device_open\n");
	device_status_t rc = suunto_vyper_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	suunto_vyper_device_set_delay (device, delay);

	dc_buffer_t *buffer = dc_buffer_new (0);

	message ("device_dump\n");
	rc = device_dump (device, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory.");
		dc_buffer_free (buffer);
		device_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (dc_buffer_get_data (buffer), sizeof (unsigned char), dc_buffer_get_size (buffer), fp);
		fclose (fp);
	}

	dc_buffer_free (buffer);

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


int main(int argc, char *argv[])
{
	message_set_logfile ("VYPER.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif
	
	unsigned int delay = 500;

	if (argc > 2) {
		name = argv[1];
		delay = atoi (argv[2]);
	} else if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s, DELAY=%i\n", name, delay);

	device_status_t a = test_dump_sdm (name, delay);
	device_status_t b = test_dump_memory (name, delay, "VYPER.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_sdm:    %s\n", errmsg (a));
	message ("test_dump_memory: %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
