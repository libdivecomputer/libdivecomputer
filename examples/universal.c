/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <suunto.h>
#include <reefnet.h>
#include <uwatec.h>
#include <oceanic.h>
#include <mares.h>
#include <hw.h>
#include <utils.h>

typedef struct backend_table_t {
	const char *name;
	device_type_t type;
} backend_table_t;

static const backend_table_t g_backends[] = {
	{"solution",	DEVICE_TYPE_SUUNTO_SOLUTION},
	{"eon",			DEVICE_TYPE_SUUNTO_EON},
	{"vyper",		DEVICE_TYPE_SUUNTO_VYPER},
	{"vyper2",		DEVICE_TYPE_SUUNTO_VYPER2},
	{"d9",			DEVICE_TYPE_SUUNTO_D9},
	{"aladin",		DEVICE_TYPE_UWATEC_ALADIN},
	{"memomouse",	DEVICE_TYPE_UWATEC_MEMOMOUSE},
	{"smart",		DEVICE_TYPE_UWATEC_SMART},
	{"sensus",		DEVICE_TYPE_REEFNET_SENSUS},
	{"sensuspro",	DEVICE_TYPE_REEFNET_SENSUSPRO},
	{"sensusultra",	DEVICE_TYPE_REEFNET_SENSUSULTRA},
	{"vtpro",		DEVICE_TYPE_OCEANIC_VTPRO},
	{"veo250",		DEVICE_TYPE_OCEANIC_VEO250},
	{"atom2",		DEVICE_TYPE_OCEANIC_ATOM2},
	{"nemo",		DEVICE_TYPE_MARES_NEMO},
	{"puck",		DEVICE_TYPE_MARES_PUCK},
	{"ostc",		DEVICE_TYPE_HW_OSTC}
};

static device_type_t
lookup_type (const char *name)
{
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		if (strcmp (name, g_backends[i].name) == 0)
			return g_backends[i].type;
	}

	return DEVICE_TYPE_NULL;
}

static const char *
lookup_name (device_type_t type)
{
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		if (g_backends[i].type == type)
			return g_backends[i].name;
	}

	return NULL;
}

static void
event_cb (device_t *device, device_event_t event, const void *data, void *userdata)
{
	const device_progress_t *progress = (device_progress_t *) data;
	const device_devinfo_t *devinfo = (device_devinfo_t *) data;

	switch (event) {
	case DEVICE_EVENT_WAITING:
		message ("Event: waiting for user action\n");
		break;
	case DEVICE_EVENT_PROGRESS:
		message ("Event: progress %3.2f%% (%u/%u)\n",
			100.0 * (double) progress->current / (double) progress->maximum,
			progress->current, progress->maximum);
		break;
	case DEVICE_EVENT_DEVINFO:
		message ("Event: model=%u (0x%08x), firmware=%u (0x%08x), serial=%u (0x%08x)\n",
			devinfo->model, devinfo->model,
			devinfo->firmware, devinfo->firmware,
			devinfo->serial, devinfo->serial);
		break;
	default:
		break;
	}
}

static int
dive_cb (const unsigned char *data, unsigned int size, void *userdata)
{
	static unsigned int count = 0;

	count++;

	message ("Dive: number=%u, size=%u\n", count, size);

	return 1;
}


static const char*
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

static void
usage (const char *filename)
{
#ifndef _MSC_VER
	fprintf (stderr, "Usage:\n\n");
	fprintf (stderr, "   %s [options] devname\n\n", filename);
	fprintf (stderr, "Options:\n\n");
	fprintf (stderr, "   -b name        Set backend name (required).\n");
	fprintf (stderr, "   -l logfile     Set logfile.\n");
	fprintf (stderr, "   -o output      Set output filename.\n");
	fprintf (stderr, "   -d             Download dives only.\n");
	fprintf (stderr, "   -m             Download memory dump only.\n");
	fprintf (stderr, "   -h             Show this help message.\n\n");
#else
	fprintf (stderr, "Usage:\n\n");
	fprintf (stderr, "   %s backend devname\n\n", filename);
#endif

	fprintf (stderr, "Supported backends:\n\n");
	unsigned int nbackends = sizeof (g_backends) / sizeof (g_backends[0]);
	for (unsigned int i = 0; i < nbackends; ++i) {
		fprintf (stderr, "%s", g_backends[i].name);
		if (i != nbackends - 1)
			fprintf (stderr, ", ");
		else
			fprintf (stderr, "\n\n");
	}
}


static device_status_t
dowork (device_type_t backend, const char *devname, const char *filename, int memory, int dives)
{
	device_status_t rc = DEVICE_STATUS_SUCCESS;

	// Open the device.
	message ("Opening the device (%s, %s).\n",
		lookup_name (backend), devname ? devname : "null");
	device_t *device = NULL;
	switch (backend) {
	case DEVICE_TYPE_SUUNTO_SOLUTION:
		rc = suunto_solution_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_EON:
		rc = suunto_eon_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER:
		rc = suunto_vyper_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_VYPER2:
		rc = suunto_vyper2_device_open (&device, devname);
		break;
	case DEVICE_TYPE_SUUNTO_D9:
		rc = suunto_d9_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_ALADIN:
		rc = uwatec_aladin_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_device_open (&device, devname);
		break;
	case DEVICE_TYPE_UWATEC_SMART:
		rc = uwatec_smart_device_open (&device);
		break;
	case DEVICE_TYPE_REEFNET_SENSUS:
		rc = reefnet_sensus_device_open (&device, devname);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_device_open (&device, devname);
		break;
	case DEVICE_TYPE_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_VTPRO:
		rc = oceanic_vtpro_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_VEO250:
		rc = oceanic_veo250_device_open (&device, devname);
		break;
	case DEVICE_TYPE_OCEANIC_ATOM2:
		rc = oceanic_atom2_device_open (&device, devname);
		break;
	case DEVICE_TYPE_MARES_NEMO:
		rc = mares_nemo_device_open (&device, devname);
		break;
	case DEVICE_TYPE_MARES_PUCK:
		rc = mares_puck_device_open (&device, devname);
		break;
	case DEVICE_TYPE_HW_OSTC:
		rc = hw_ostc_device_open (&device, devname);
		break;
	default:
		rc = DEVICE_STATUS_ERROR;
		break;
	}
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening device.");
		return rc;
	}

	// Register the event handler.
	message ("Registering the event handler.\n");
	int events = DEVICE_EVENT_WAITING | DEVICE_EVENT_PROGRESS | DEVICE_EVENT_DEVINFO;
	rc = device_set_events (device, events, event_cb, NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error registering the event handler.");
		device_close (device);
		return rc;
	}

	if (memory) {
		// Allocate a memory buffer.
		dc_buffer_t *buffer = dc_buffer_new (0);

		// Download the memory dump.
		message ("Downloading the memory dump.\n");
		rc = device_dump (device, buffer);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Error downloading the memory dump.");
			dc_buffer_free (buffer);
			device_close (device);
			return rc;
		}

		// Write the memory dump to disk.
		FILE* fp = fopen (filename, "wb");
		if (fp != NULL) {
			fwrite (dc_buffer_get_data (buffer), 1, dc_buffer_get_size (buffer), fp);
			fclose (fp);
		}

		// Free the memory buffer.
		dc_buffer_free (buffer);
	}

	if (dives) {
		// Download the dives.
		message ("Downloading the dives.\n");
		rc = device_foreach (device, dive_cb, NULL);
		if (rc != DEVICE_STATUS_SUCCESS) {
			WARNING ("Error downloading the dives.");
			device_close (device);
			return rc;
		}
	}

	// Close the device.
	message ("Closing the device.\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error closing the device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


int
main (int argc, char *argv[])
{
	// Default values.
	device_type_t backend = DEVICE_TYPE_NULL;
	const char *logfile = "output.log";
	const char *filename = "output.bin";
	const char *devname = NULL;
	int memory = 1, dives = 1;

#ifndef _MSC_VER
	// Parse command-line options.
	int opt = 0;
	while ((opt = getopt (argc, argv, "b:l:o:mdh")) != -1) {
		switch (opt) {
		case 'b':
			backend = lookup_type (optarg);
			break;
		case 'l':
			logfile = optarg;
			break;
		case 'o':
			filename = optarg;
			break;
		case 'm':
			dives = 0;
			break;
		case 'd':
			memory = 0;
			break;
		case '?':
		case 'h':
		default:
			usage (argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		devname = argv[optind];
#else
	if (argc > 1)
		backend = lookup_type (argv[1]);

	if (argc > 2)
		devname = argv[2];
#endif

	// The backend is a mandatory argument.
	if (backend == DEVICE_TYPE_NULL) {
		usage (argv[0]);
		return EXIT_FAILURE;
	}

	message_set_logfile (logfile);

	device_status_t rc = dowork (backend, devname, filename, memory, dives);
	message ("Result: %s\n", errmsg (rc));

	message_set_logfile (NULL);

	return rc != DEVICE_STATUS_SUCCESS ? EXIT_FAILURE : EXIT_SUCCESS;
}
