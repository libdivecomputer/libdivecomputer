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
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/iterator.h>

#include "dctool.h"

static int
dctool_list_run (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *dummy)
{
	// Default option values.
	unsigned int help = 0;

	// Parse the command-line options.
	int opt = 0;
	const char *optstring = "h";
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

	argc -= optind;
	argv += optind;

	// Show help message.
	if (help) {
		dctool_command_showhelp (&dctool_list);
		return EXIT_SUCCESS;
	}

	dc_iterator_t *iterator = NULL;
	dc_descriptor_t *descriptor = NULL;
	dc_descriptor_iterator (&iterator);
	while (dc_iterator_next (iterator, &descriptor) == DC_STATUS_SUCCESS) {
		printf ("%s %s\n",
			dc_descriptor_get_vendor (descriptor),
			dc_descriptor_get_product (descriptor));
		dc_descriptor_free (descriptor);
	}
	dc_iterator_free (iterator);

	return EXIT_SUCCESS;
}

const dctool_command_t dctool_list = {
	dctool_list_run,
	DCTOOL_CONFIG_NONE,
	"list",
	"List supported devices",
	"Usage:\n"
	"   dctool list [options]\n"
	"\n"
	"Options:\n"
#ifdef HAVE_GETOPT_LONG
	"   -h, --help   Show help message\n"
#else
	"   -h   Show help message\n"
#endif
};
