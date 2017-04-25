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

#include <libdivecomputer/datetime.h>

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

static time_t
dc_timegm (struct tm *tm)
{
#if defined(HAVE_TIMEGM)
	return timegm (tm);
#elif defined (HAVE__MKGMTIME)
	return _mkgmtime (tm);
#else
	static const unsigned int mdays[] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	};

	if (tm == NULL ||
		tm->tm_mon  < 0 || tm->tm_mon  > 11 ||
		tm->tm_mday < 1 || tm->tm_mday > 31 ||
		tm->tm_hour < 0 || tm->tm_hour > 23 ||
		tm->tm_min  < 0 || tm->tm_min  > 59 ||
		tm->tm_sec  < 0 || tm->tm_sec  > 60)
		return (time_t) -1;

	/* Number of leap days since 1970-01-01. */
	int year = tm->tm_year + 1900 - (tm->tm_mon < 2);
	int leapdays =
		((year / 4) - (year / 100) + (year / 400)) -
		((1969 / 4) - (1969 / 100) + (1969 / 400));

	time_t result = 0;
	result += (tm->tm_year - 70) * 365;
	result += leapdays;
	result += mdays[tm->tm_mon];
	result += tm->tm_mday - 1;
	result *= 24;
	result += tm->tm_hour;
	result *= 60;
	result += tm->tm_min;
	result *= 60;
	result += tm->tm_sec;
	return result;
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
	int offset = 0;

	struct tm tm;
	if (dc_localtime_r (&t, &tm) == NULL)
		return NULL;

#ifdef HAVE_STRUCT_TM_TM_GMTOFF
	offset = tm.tm_gmtoff;
#else
	struct tm tmp = tm;
	time_t t_local = dc_timegm (&tmp);
	if (t_local == (time_t) -1)
		return NULL;

	offset = t_local - t;
#endif

	if (result) {
		result->year = tm.tm_year + 1900;
		result->month = tm.tm_mon + 1;
		result->day = tm.tm_mday;
		result->hour = tm.tm_hour;
		result->minute = tm.tm_min;
		result->second = tm.tm_sec;
		result->timezone = offset;
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
		result->timezone = 0;
	}

	return result;
}

dc_ticks_t
dc_datetime_mktime (const dc_datetime_t *dt)
{
	if (dt == NULL)
		return -1;

	struct tm tm;
	tm.tm_year = dt->year - 1900;
	tm.tm_mon = dt->month - 1;
	tm.tm_mday = dt->day;
	tm.tm_hour = dt->hour;
	tm.tm_min = dt->minute;
	tm.tm_sec = dt->second;
	tm.tm_isdst = 0;

	time_t t = dc_timegm (&tm);
	if (t == (time_t) -1)
		return t;

	if (dt->timezone != DC_TIMEZONE_NONE) {
		t -= dt->timezone;
	}

	return t;
}
