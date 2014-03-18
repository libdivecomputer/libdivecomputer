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

#define WARNING(expr) message ("%s:%d: %s\n", __FILE__, __LINE__, expr)

int message (const char* fmt, ...);

void message_set_logfile (const char* filename);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* EXAMPLES_UTILS_H */
