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
#include <libdivecomputer/parser.h>

#include <libdivecomputer/suunto.h>
#include <libdivecomputer/reefnet.h>
#include <libdivecomputer/uwatec.h>
#include <libdivecomputer/oceanic.h>
#include <libdivecomputer/mares.h>
#include <libdivecomputer/hw.h>
#include <libdivecomputer/cressi.h>
#include <libdivecomputer/zeagle.h>
#include <libdivecomputer/atomics.h>
#include <libdivecomputer/shearwater.h>
#include <libdivecomputer/diverite.h>
#include <libdivecomputer/citizen.h>
#include <libdivecomputer/divesystem.h>

#include "dctool.h"
#include "output.h"
#include "common.h"
#include "utils.h"

#define REACTPROWHITE 0x4354

static dc_status_t
parse (dc_buffer_t *buffer, dc_context_t *context, dc_descriptor_t *descriptor, unsigned int devtime, dc_ticks_t systime, dctool_output_t *output)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_parser_t *parser = NULL;
	dc_family_t family = dc_descriptor_get_type (descriptor);
	unsigned int model = dc_descriptor_get_model (descriptor);
	unsigned char *data = dc_buffer_get_data (buffer);
	unsigned int size = dc_buffer_get_size (buffer);

	// Create the parser.
	message ("Creating the parser.\n");
	switch (family) {
	case DC_FAMILY_SUUNTO_SOLUTION:
		rc = suunto_solution_parser_create (&parser, context);
		break;
	case DC_FAMILY_SUUNTO_EON:
		rc = suunto_eon_parser_create (&parser, context, 0);
		break;
	case DC_FAMILY_SUUNTO_VYPER:
		if (model == 0x01)
			rc = suunto_eon_parser_create (&parser, context, 1);
		else
			rc = suunto_vyper_parser_create (&parser, context);
		break;
	case DC_FAMILY_SUUNTO_VYPER2:
	case DC_FAMILY_SUUNTO_D9:
		rc = suunto_d9_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_SUUNTO_EONSTEEL:
		rc = suunto_eonsteel_parser_create(&parser, context, model);
		break;
	case DC_FAMILY_UWATEC_ALADIN:
	case DC_FAMILY_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_parser_create (&parser, context, devtime, systime);
		break;
	case DC_FAMILY_UWATEC_SMART:
	case DC_FAMILY_UWATEC_MERIDIAN:
		rc = uwatec_smart_parser_create (&parser, context, model, devtime, systime);
		break;
	case DC_FAMILY_REEFNET_SENSUS:
		rc = reefnet_sensus_parser_create (&parser, context, devtime, systime);
		break;
	case DC_FAMILY_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_parser_create (&parser, context, devtime, systime);
		break;
	case DC_FAMILY_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_parser_create (&parser, context, devtime, systime);
		break;
	case DC_FAMILY_OCEANIC_VTPRO:
		rc = oceanic_vtpro_parser_create (&parser, context);
		break;
	case DC_FAMILY_OCEANIC_VEO250:
		rc = oceanic_veo250_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_OCEANIC_ATOM2:
		if (model == REACTPROWHITE)
			rc = oceanic_veo250_parser_create (&parser, context, model);
		else
			rc = oceanic_atom2_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_MARES_NEMO:
	case DC_FAMILY_MARES_PUCK:
		rc = mares_nemo_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_MARES_DARWIN:
		rc = mares_darwin_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_MARES_ICONHD:
		rc = mares_iconhd_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_HW_OSTC:
		rc = hw_ostc_parser_create (&parser, context, 0);
		break;
	case DC_FAMILY_HW_FROG:
	case DC_FAMILY_HW_OSTC3:
		rc = hw_ostc_parser_create (&parser, context, 1);
		break;
	case DC_FAMILY_CRESSI_EDY:
	case DC_FAMILY_ZEAGLE_N2ITION3:
		rc = cressi_edy_parser_create (&parser, context, model);
		break;
	case DC_FAMILY_CRESSI_LEONARDO:
		rc = cressi_leonardo_parser_create (&parser, context);
		break;
	case DC_FAMILY_ATOMICS_COBALT:
		rc = atomics_cobalt_parser_create (&parser, context);
		break;
	case DC_FAMILY_SHEARWATER_PREDATOR:
		rc = shearwater_predator_parser_create (&parser, context);
		break;
	case DC_FAMILY_SHEARWATER_PETREL:
		rc = shearwater_petrel_parser_create (&parser, context);
		break;
	case DC_FAMILY_DIVERITE_NITEKQ:
		rc = diverite_nitekq_parser_create (&parser, context);
		break;
	case DC_FAMILY_CITIZEN_AQUALAND:
		rc = citizen_aqualand_parser_create (&parser, context);
		break;
	case DC_FAMILY_DIVESYSTEM_IDIVE:
		rc = divesystem_idive_parser_create2 (&parser, context, model);
		break;
	default:
		rc = DC_STATUS_INVALIDARGS;
		break;
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error creating the parser.");
		goto cleanup;
	}

	// Register the data.
	message ("Registering the data.\n");
	rc = dc_parser_set_data (parser, data, size);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error registering the data.");
		goto cleanup;
	}

	// Parse the dive data.
	message ("Parsing the dive data.\n");
	rc = dctool_output_write (output, parser, data, size, NULL, 0);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR ("Error parsing the dive data.");
		goto cleanup;
	}

cleanup:
	dc_parser_destroy (parser);
	return rc;
}

static int
dctool_parse_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor)
{
	// Default values.
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_buffer_t *buffer = NULL;
	dctool_output_t *output = NULL;
	dctool_units_t units = DCTOOL_UNITS_METRIC;

	// Default option values.
	unsigned int help = 0;
	const char *filename = NULL;
	unsigned int devtime = 0;
	dc_ticks_t systime = 0;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "ho:d:s:u:";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"output",      required_argument, 0, 'o'},
		{"devtime",     required_argument, 0, 'd'},
		{"systime",     required_argument, 0, 's'},
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
		case 'o':
			filename = optarg;
			break;
		case 'd':
			devtime = strtoul (optarg, NULL, 0);
			break;
		case 's':
			systime = strtoll (optarg, NULL, 0);
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
		dctool_command_showhelp (&dctool_parse);
		return EXIT_SUCCESS;
	}

	// Create the output.
	output = dctool_xml_output_new (filename, units);
	if (output == NULL) {
		message ("Failed to create the output.\n");
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	for (unsigned int i = 0; i < argc; ++i) {
		// Read the input file.
		buffer = dctool_file_read (argv[i]);
		if (buffer == NULL) {
			message ("Failed to open the input file.\n");
			exitcode = EXIT_FAILURE;
			goto cleanup;
		}

		// Parse the dive.
		status = parse (buffer, context, descriptor, devtime, systime, output);
		if (status != DC_STATUS_SUCCESS) {
			message ("ERROR: %s\n", dctool_errmsg (status));
			exitcode = EXIT_FAILURE;
			goto cleanup;
		}

		// Cleanup.
		dc_buffer_free (buffer);
		buffer = NULL;
	}

cleanup:
	dc_buffer_free (buffer);
	dctool_output_free (output);
	return exitcode;
}

const dctool_command_t dctool_parse = {
	dctool_parse_run,
	DCTOOL_CONFIG_DESCRIPTOR,
	"parse",
	"Parse previously downloaded dives",
	"Usage:\n"
	"   dctool parse [options] <filename>\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help                 Show help message\n"
	"   -o, --output <filename>    Output filename\n"
	"   -d, --devtime <timestamp>  Device time\n"
	"   -s, --systime <timestamp>  System time\n"
	"   -u, --units <units>        Set units (metric or imperial)\n"
#else
	"   -h              Show help message\n"
	"   -o <filename>   Output filename\n"
	"   -d <devtime>    Device time\n"
	"   -s <systime>    System time\n"
	"   -u <units>      Set units (metric or imperial)\n"
#endif
};
