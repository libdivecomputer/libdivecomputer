/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Jef Driesen
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#else
#include <time.h>
#include <errno.h>
#endif

#include "platform.h"

int
dc_platform_sleep (unsigned int milliseconds)
{
#ifdef _WIN32
	Sleep (milliseconds);
#else
	struct timespec ts;
	ts.tv_sec  = (milliseconds / 1000);
	ts.tv_nsec = (milliseconds % 1000) * 1000000;

	while (nanosleep (&ts, &ts) != 0) {
		if (errno != EINTR ) {
			return -1;
		}
	}
#endif

	return 0;
}
