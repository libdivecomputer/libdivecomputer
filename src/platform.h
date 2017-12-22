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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef _WIN32
#define DC_PRINTF_SIZE "%Iu"
#else
#define DC_PRINTF_SIZE "%zu"
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
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

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_PLATFORM_H */
