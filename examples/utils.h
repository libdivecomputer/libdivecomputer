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

#ifndef EXAMPLES_UTILS_H
#define EXAMPLES_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef _MSC_VER
#define snprintf _snprintf
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define FUNCTION __func__
#else
#define FUNCTION __FUNCTION__
#endif

#if defined(__GNUC__)
#define ATTR_FORMAT_PRINTF(a,b) __attribute__((format(printf, a, b)))
#else
#define ATTR_FORMAT_PRINTF(a,b)
#endif

#define WARNING(expr) message("WARNING: %s [in %s:%d (%s)]\n", expr, __FILE__, __LINE__, FUNCTION)
#define ERROR(expr)   message("ERROR: %s [in %s:%d (%s)]\n", expr, __FILE__, __LINE__, FUNCTION)

int message (const char* fmt, ...) ATTR_FORMAT_PRINTF(1, 2);

void message_set_logfile (const char* filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* EXAMPLES_UTILS_H */
