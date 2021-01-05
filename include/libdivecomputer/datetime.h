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

#ifndef DC_DATETIME_H
#define DC_DATETIME_H

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DC_TIMEZONE_NONE INT_MIN

#if defined (_WIN32) && !defined (__GNUC__)
typedef __int64 dc_ticks_t;
#else
typedef long long int dc_ticks_t;
#endif

typedef struct dc_datetime_t {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int timezone;
} dc_datetime_t;

dc_ticks_t
dc_datetime_now (void);

dc_datetime_t *
dc_datetime_localtime (dc_datetime_t *result,
                       dc_ticks_t ticks);

dc_datetime_t *
dc_datetime_gmtime (dc_datetime_t *result,
                    dc_ticks_t ticks);

dc_ticks_t
dc_datetime_mktime (const dc_datetime_t *dt);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_DATETIME_H */
