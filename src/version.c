/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#include <libdivecomputer/version.h>

#ifdef HAVE_VERSION_SUFFIX
#include "revision.h"
#endif

const char *
dc_version (dc_version_t *version)
{
	if (version) {
		version->major = DC_VERSION_MAJOR;
		version->minor = DC_VERSION_MINOR;
		version->micro = DC_VERSION_MICRO;
	}

#ifdef HAVE_VERSION_SUFFIX
	return DC_VERSION " (" DC_VERSION_REVISION ")";
#else
	return DC_VERSION;
#endif
}

int
dc_version_check (unsigned int major, unsigned int minor, unsigned int micro)
{
	return DC_VERSION_CHECK (major,minor,micro);
}
