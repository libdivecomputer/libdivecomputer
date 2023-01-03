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

#include <stdio.h>

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

int
dc_platform_vsnprintf (char *str, size_t size, const char *format, va_list ap)
{
	int n = 0;

	if (size == 0)
		return -1;

#ifdef _MSC_VER
	/*
	 * The non-standard vsnprintf implementation provided by MSVC doesn't null
	 * terminate the string and returns a negative value if the destination
	 * buffer is too small.
	 */
	n = _vsnprintf (str, size - 1, format, ap);
	if (n == size - 1 || n < 0)
		str[size - 1] = 0;
#else
	/*
	 * The C99 vsnprintf function will always null terminate the string. If the
	 * destination buffer is too small, the return value is the number of
	 * characters that would have been written if the buffer had been large
	 * enough.
	 */
	n = vsnprintf (str, size, format, ap);
	if (n >= 0 && (size_t) n >= size)
		n = -1;
#endif

	return n;
}

int
dc_platform_snprintf (char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int n = 0;

	va_start (ap, format);
	n = dc_platform_vsnprintf (str, size, format, ap);
	va_end (ap);

	return n;
}
