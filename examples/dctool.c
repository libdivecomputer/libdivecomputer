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
#include <string.h>
#include <stdio.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "dctool.h"
#include "utils.h"

#if defined(__GLIBC__) || defined(__MINGW32__)
#define NOPERMUTATION "+"
#define RESET 0
#else
#define NOPERMUTATION ""
#define RESET 1
#endif

static const dctool_command_t *g_commands[] = {
	NULL
};

const dctool_command_t *
dctool_command_find (const char *name)
{
	if (name == NULL)
		return NULL;

	size_t i = 0;
	while (g_commands[i] != NULL) {
		if (strcmp(g_commands[i]->name, name) == 0) {
			break;
		}
		i++;
	}

	return g_commands[i];
}

static void
logfunc (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *msg, void *userdata)
{
	const char *loglevels[] = {"NONE", "ERROR", "WARNING", "INFO", "DEBUG", "ALL"};

	if (loglevel == DC_LOGLEVEL_ERROR || loglevel == DC_LOGLEVEL_WARNING) {
		message ("%s: %s [in %s:%d (%s)]\n", loglevels[loglevel], msg, file, line, function);
	} else {
		message ("%s: %s\n", loglevels[loglevel], msg);
	}
}

int
main (int argc, char *argv[])
{
	int exitcode = EXIT_SUCCESS;
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_context_t *context = NULL;

	// Default option values.
	unsigned int help = 0;
	dc_loglevel_t loglevel = DC_LOGLEVEL_WARNING;
	const char *logfile = NULL;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = NOPERMUTATION "hl:qv";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"logfile",     required_argument, 0, 'l'},
		{"quiet",       no_argument,       0, 'q'},
		{"verbose",     no_argument,       0, 'v'},
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
		case 'l':
			logfile = optarg;
			break;
		case 'q':
			loglevel = DC_LOGLEVEL_NONE;
			break;
		case 'v':
			loglevel++;
			break;
		default:
			return EXIT_FAILURE;
		}
	}

	// Skip the processed arguments.
	argc -= optind;
	argv += optind;
	optind = RESET;

	// Show help message.
	if (help || argv[0] == NULL) {
		printf (
			"A simple command line interface for the libdivecomputer library\n"
			"\n"
			"Usage:\n"
			"   dctool [options] <command> [<args>]\n"
			"\n"
			"Options:\n"
#ifdef HAVE_GETOPT_LONG
			"   -h, --help                Show help message\n"
			"   -l, --logfile <logfile>   Logfile\n"
			"   -q, --quiet               Quiet mode\n"
			"   -v, --verbose             Verbose mode\n"
#else
			"   -h             Show help message\n"
			"   -l <logfile>   Logfile\n"
			"   -q             Quiet mode\n"
			"   -v             Verbose mode\n"
#endif
			"\n");
		return EXIT_SUCCESS;
	}

	// Try to find the command.
	const dctool_command_t *command = dctool_command_find (argv[0]);
	if (command == NULL) {
		message ("Unknown command %s.\n", argv[0]);
		return EXIT_FAILURE;
	}

	// Initialize the logfile.
	message_set_logfile (logfile);

	// Initialize a library context.
	status = dc_context_new (&context);
	if (status != DC_STATUS_SUCCESS) {
		exitcode = EXIT_FAILURE;
		goto cleanup;
	}

	// Setup the logging.
	dc_context_set_loglevel (context, loglevel);
	dc_context_set_logfunc (context, logfunc, NULL);

	// Execute the command.
	exitcode = command->run (argc, argv, context);

cleanup:
	dc_context_free (context);
	message_set_logfile (NULL);
	return exitcode;
}
