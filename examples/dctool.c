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

int
main (int argc, char *argv[])
{
	// Default option values.
	unsigned int help = 0;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = NOPERMUTATION "h";
#ifdef HAVE_GETOPT_LONG
	struct option options[] = {
		{"help",        no_argument,       0, 'h'},
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
			"   -h, --help   Show help message\n"
#else
			"   -h   Show help message\n"
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

	// Execute the command.
	return command->run (argc, argv);
}
