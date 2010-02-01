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

#include <time.h>

#include "datetime.h"

static struct tm *
dc_localtime_r (const time_t *t, struct tm *tm)
{
#ifdef HAVE_LOCALTIME_R
	return localtime_r (t, tm);
#else
	struct tm *p = localtime (t);
	if (p == NULL)
		return NULL;

	if (tm)
		*tm = *p;

	return tm;
#endif
}

static struct tm *
dc_gmtime_r (const time_t *t, struct tm *tm)
{
#ifdef HAVE_GMTIME_R
	return gmtime_r (t, tm);
#else
	struct tm *p = gmtime (t);
	if (p == NULL)
		return NULL;

	if (tm)
		*tm = *p;

	return tm;
#endif
}

dc_ticks_t
dc_datetime_now (void)
{
	return time (NULL);
}

dc_datetime_t *
dc_datetime_localtime (dc_datetime_t *result,
                       dc_ticks_t ticks)
{
	time_t t = ticks;

	struct tm tm;
	if (dc_localtime_r (&t, &tm) == NULL)
		return NULL;

	if (result) {
		result->year = tm.tm_year + 1900;
		result->month = tm.tm_mon + 1;
		result->day = tm.tm_mday;
		result->hour = tm.tm_hour;
		result->minute = tm.tm_min;
		result->second = tm.tm_sec;
	}

	return result;
}

dc_datetime_t *
dc_datetime_gmtime (dc_datetime_t *result,
                    dc_ticks_t ticks)
{
	time_t t = ticks;

	struct tm tm;
	if (dc_gmtime_r (&t, &tm) == NULL)
		return NULL;

	if (result) {
		result->year = tm.tm_year + 1900;
		result->month = tm.tm_mon + 1;
		result->day = tm.tm_mday;
		result->hour = tm.tm_hour;
		result->minute = tm.tm_min;
		result->second = tm.tm_sec;
	}

	return result;
}
