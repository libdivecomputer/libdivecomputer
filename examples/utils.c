/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <libdivecomputer/datetime.h>
#include <libdivecomputer/version.h>

#include "utils.h"

static FILE* g_logfile = NULL;

static unsigned char g_lastchar = '\n';

#ifdef _WIN32
	#include <windows.h>
	static LARGE_INTEGER g_timestamp, g_frequency;
#else
	#include <sys/time.h>
	static struct timeval g_timestamp;
#endif

int message (const char* fmt, ...)
{
	va_list ap;

	if (g_logfile) {
		if (g_lastchar == '\n') {
#ifdef _WIN32
			LARGE_INTEGER now, timestamp;
			QueryPerformanceCounter(&now);
			timestamp.QuadPart = now.QuadPart - g_timestamp.QuadPart;
			timestamp.QuadPart *= 1000000;
			timestamp.QuadPart /= g_frequency.QuadPart;
			fprintf (g_logfile, "[%I64i.%06I64i] ", timestamp.QuadPart / 1000000, timestamp.QuadPart % 1000000);
#else
			struct timeval now, timestamp;
			gettimeofday (&now, NULL);
			timersub (&now, &g_timestamp, &timestamp);
			fprintf (g_logfile, "[%lli.%06lli] ", (long long)timestamp.tv_sec, (long long)timestamp.tv_usec);
#endif
		}

		size_t len = strlen (fmt);
		if (len > 0)
			g_lastchar = fmt[len - 1];
		else
			g_lastchar = 0;

		va_start (ap, fmt);
		vfprintf (g_logfile, fmt, ap);
		va_end (ap);
	}

	va_start (ap, fmt);
	int rc = vfprintf (stderr, fmt, ap);
	va_end (ap);

	return rc;
}

void message_set_logfile (const char* filename)
{
	if (g_logfile) {
		fclose (g_logfile);
		g_logfile = NULL;
	}

	if (filename)
		g_logfile = fopen (filename, "w");

	if (g_logfile) {
		g_lastchar = '\n';
#ifdef _WIN32
		QueryPerformanceFrequency(&g_frequency);
		QueryPerformanceCounter(&g_timestamp);
#else
		gettimeofday (&g_timestamp, NULL);
#endif
		dc_datetime_t dt = {0};
		dc_ticks_t now = dc_datetime_now ();
		dc_datetime_gmtime (&dt, now);
		message ("DATETIME %u-%02u-%02uT%02u:%02u:%02uZ (%lu)\n",
			dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
			(unsigned long) now);
		message ("VERSION %s\n", dc_version (NULL));
	}
}
