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
#include <time.h>	// time

#include "reefnet_sensusultra.h"
#include "utils.h"

device_status_t
test_dump_memory_dives (const char* name, const char* filename)
{
	device_t *device = NULL;

	message ("reefnet_sensusultra_device_open\n");
	device_status_t rc = reefnet_sensusultra_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	time_t now = time (NULL);
	char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

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
test_dump_memory_data (const char* name, const char* filename)
{
	device_t *device = NULL;

	message ("reefnet_sensusultra_device_open\n");
	device_status_t rc = reefnet_sensusultra_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	time_t now = time (NULL);
	char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

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


device_status_t
test_dump_memory_user (const char* name, const char* filename)
{
	device_t *device = NULL;
	unsigned char data[REEFNET_SENSUSULTRA_MEMORY_USER_SIZE] = {0};

	message ("reefnet_sensusultra_device_open\n");
	device_status_t rc = reefnet_sensusultra_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	time_t now = time (NULL);
	char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

	message ("reefnet_sensusultra_device_read_user\n");
	rc = reefnet_sensusultra_device_read_user (device, data, sizeof (data));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory.");
		device_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


const char*
errmsg (device_status_t rc)
{
	switch (rc) {
	case DEVICE_STATUS_SUCCESS:
		return "Success";
	case DEVICE_STATUS_UNSUPPORTED:
		return "Unsupported operation";
	case DEVICE_STATUS_TYPE_MISMATCH:
		return "Device type mismatch";
	case DEVICE_STATUS_ERROR:
		return "Generic error";
	case DEVICE_STATUS_IO:
		return "Input/output error";
	case DEVICE_STATUS_MEMORY:
		return "Memory error";
	case DEVICE_STATUS_PROTOCOL:
		return "Protocol error";
	case DEVICE_STATUS_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}


int main(int argc, char *argv[])
{
	message_set_logfile ("SENSUSULTRA.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	device_status_t a = test_dump_memory_data (name, "SENSUSULTRA_DATA.DMP");
	device_status_t b = test_dump_memory_user (name, "SENSUSULTRA_USER.DMP");
	device_status_t c = test_dump_memory_dives (name, "SENSUSULTRA_DIVES.DMP");

	message ("SUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory_data:     %s\n", errmsg (a));
	message ("test_dump_memory_user:     %s\n", errmsg (b));
	message ("test_dump_memory_dives:    %s\n", errmsg (c));

	message_set_logfile (NULL);

	return 0;
}
