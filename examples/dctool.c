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
#include <signal.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>

#include "common.h"
#include "dctool.h"
#include "utils.h"

#if defined(__GLIBC__) || defined(__MINGW32__)
#define RESET 0
#else
#define RESET 1
#endif

#if defined(__GLIBC__) || defined(__MINGW32__) || defined(BSD)
#define NOPERMUTATION "+"
#else
#define NOPERMUTATION ""
#endif

static const dctool_command_t *g_commands[] = {
	&dctool_help,
	&dctool_version,
	&dctool_list,
	&dctool_download,
	&dctool_dump,
	&dctool_parse,
	&dctool_read,
	&dctool_write,
	&dctool_fwupdate,
	NULL
};

static volatile sig_atomic_t g_cancel = 0;

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

void
dctool_command_showhelp (const dctool_command_t *command)
{
	if (command == NULL) {
		unsigned int maxlength = 0;
		for (size_t i = 0; g_commands[i] != NULL; ++i) {
			unsigned int length = strlen (g_commands[i]->name);
			if (length > maxlength)
				maxlength = length;
		}
		printf (
			"A simple command line interface for the libdivecomputer library\n"
			"\n"
			"Usage:\n"
			"   dctool [options] <command> [<args>]\n"
			"\n"
			"Options:\n"
#ifdef HAVE_GETOPT_LONG
			"   -h, --help                Show help message\n"
			"   -d, --device <device>     Device name\n"
			"   -f, --family <family>     Device family type\n"
			"   -m, --model <model>       Device model number\n"
			"   -l, --logfile <logfile>   Logfile\n"
			"   -q, --quiet               Quiet mode\n"
			"   -v, --verbose             Verbose mode\n"
#else
			"   -h             Show help message\n"
			"   -d <device>    Device name\n"
			"   -f <family>    Family type\n"
			"   -m <model>     Model number\n"
			"   -l <logfile>   Logfile\n"
			"   -q             Quiet mode\n"
			"   -v             Verbose mode\n"
#endif
			"\n"
			"Available commands:\n");
		for (size_t i = 0; g_commands[i] != NULL; ++i) {
			printf ("   %-*s%s\n", maxlength + 3, g_commands[i]->name, g_commands[i]->description);
		}
		printf ("\nSee 'dctool help <command>' for more information on a specific command.\n\n");
	} else {
		printf ("%s\n\n%s\n", command->description, command->usage);
	}
}

int
dctool_cancel_cb (void *userdata)
{
	return g_cancel;
}

static void
sighandler (int signum)
{
#ifndef _WIN32
	// Restore the default signal handler.
	signal (signum, SIG_DFL);
#endif

	g_cancel = 1;
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
	dc_descriptor_t *descriptor = NULL;

	// Default option values.
	unsigned int help = 0;
	dc_loglevel_t loglevel = DC_LOGLEVEL_WARNING;
	const char *logfile = NULL;
	const char *device = NULL;
	dc_family_t family = DC_FAMILY_NULL;
	unsigned int model = 0;
	unsigned int have_family = 0, have_model = 0;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = NOPERMUTATION "hd:f:m:l:qv";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
		{"device",      required_argument, 0, 'd'},
		{"family",      required_argument, 0, 'f'},
		{"model",       required_argument, 0, 'm'},
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
		case 'd':
			device = optarg;
			break;
		case 'f':
			family = dctool_family_type (optarg);
			have_family = 1;
			break;
		case 'm':
			model = strtoul (optarg, NULL, 0);
			have_model = 1;
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
#if defined(HAVE_DECL_OPTRESET) && HAVE_DECL_OPTRESET
	optreset = 1;
#endif

	// Set the default model number.
	if (have_family && !have_model) {
		model = dctool_family_model (family);
	}

	// Translate the help option into a command.
	char *argv_help[] = {(char *) "help", NULL, NULL};
	if (help || argv[0] == NULL) {
		if (argv[0]) {
			argv_help[1] = argv[0];
			argv = argv_help;
			argc = 2;
		} else {
			argv = argv_help;
			argc = 1;
		}
	}

	// Try to find the command.
	const dctool_command_t *command = dctool_command_find (argv[0]);
	if (command == NULL) {
		message ("Unknown command %s.\n", argv[0]);
		return EXIT_FAILURE;
	}

	// Setup the cancel signal handler.
	signal (SIGINT, sighandler);

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

	if (command->config & DCTOOL_CONFIG_DESCRIPTOR) {
		// Check mandatory arguments.
		if (device == NULL && family == DC_FAMILY_NULL) {
			message ("No device name or family type specified.\n");
			exitcode = EXIT_FAILURE;
			goto cleanup;
		}

		// Search for a matching device descriptor.
		status = dctool_descriptor_search (&descriptor, device, family, model);
		if (status != DC_STATUS_SUCCESS) {
			exitcode = EXIT_FAILURE;
			goto cleanup;
		}

		// Fail if no device descriptor found.
		if (descriptor == NULL) {
			if (device) {
				message ("No supported device found: %s\n",
					device);
			} else {
				message ("No supported device found: %s, 0x%X\n",
					dctool_family_name (family), model);
			}
			exitcode = EXIT_FAILURE;
			goto cleanup;
		}
	}

	// Execute the command.
	exitcode = command->run (argc, argv, context, descriptor);

cleanup:
	dc_descriptor_free (descriptor);
	dc_context_free (context);
	message_set_logfile (NULL);
	return exitcode;
}
