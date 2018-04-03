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

#ifndef DCTOOL_H
#define DCTOOL_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dctool_config_t {
	DCTOOL_CONFIG_NONE = 0,
	DCTOOL_CONFIG_DESCRIPTOR = 1,
} dctool_config_t;

typedef struct dctool_command_t {
	int (*run) (int argc, char *argv[], dc_context_t *context, dc_descriptor_t *descriptor);
	unsigned int config;
	const char *name;
	const char *description;
	const char *usage;
} dctool_command_t;

extern const dctool_command_t dctool_help;
extern const dctool_command_t dctool_version;
extern const dctool_command_t dctool_list;
extern const dctool_command_t dctool_scan;
extern const dctool_command_t dctool_download;
extern const dctool_command_t dctool_dump;
extern const dctool_command_t dctool_parse;
extern const dctool_command_t dctool_read;
extern const dctool_command_t dctool_write;
extern const dctool_command_t dctool_timesync;
extern const dctool_command_t dctool_fwupdate;

const dctool_command_t *
dctool_command_find (const char *name);

void
dctool_command_showhelp (const dctool_command_t *command);

int
dctool_cancel_cb (void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DCTOOL_H */
