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

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DC_RINGBUFFER_EMPTY 0
#define DC_RINGBUFFER_FULL  1

unsigned int
ringbuffer_normalize (unsigned int a, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_distance (unsigned int a, unsigned int b, int mode, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_increment (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_decrement (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* RINGBUFFER_H */
