/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#ifndef DC_PLATFORM_H
#define DC_PLATFORM_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#if defined(__GNUC__)
#define DC_ATTR_FORMAT_PRINTF(a,b) __attribute__((format(printf, a, b)))
#else
#define DC_ATTR_FORMAT_PRINTF(a,b)
#endif

#ifdef _WIN32
#define DC_PRINTF_SIZE "%Iu"
#define DC_FORMAT_INT64 "%I64d"
#else
#define DC_PRINTF_SIZE "%zu"
#define DC_FORMAT_INT64 "%lld"
#endif

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#if _MSC_VER < 1800
// The rint() function is only available in MSVC 2013 and later
// versions. Our replacement macro isn't entirely correct, because the
// rounding rules for halfway cases are slightly different (away from
// zero vs to even). But for our use-case, that's not a problem.
#define rint(x) ((x) >= 0.0 ? floor((x) + 0.5): ceil((x) - 0.5))
#endif
#endif

int dc_platform_sleep(unsigned int milliseconds);

/*
 * A wrapper for the vsnprintf function, which will always null terminate the
 * string and returns a negative value if the destination buffer is too small.
 */
int dc_platform_snprintf (char *str, size_t size, const char *format, ...) DC_ATTR_FORMAT_PRINTF(3, 4);
int dc_platform_vsnprintf (char *str, size_t size, const char *format, va_list ap) DC_ATTR_FORMAT_PRINTF(3, 0);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_PLATFORM_H */
