/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#ifndef DC_BUFFER_H
#define DC_BUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_buffer_t dc_buffer_t;

dc_buffer_t *
dc_buffer_new (size_t capacity);

void
dc_buffer_free (dc_buffer_t *buffer);

int
dc_buffer_clear (dc_buffer_t *buffer);

int
dc_buffer_reserve (dc_buffer_t *buffer, size_t capacity);

int
dc_buffer_resize (dc_buffer_t *buffer, size_t size);

int
dc_buffer_append (dc_buffer_t *buffer, const unsigned char data[], size_t size);

int
dc_buffer_prepend (dc_buffer_t *buffer, const unsigned char data[], size_t size);

int
dc_buffer_insert (dc_buffer_t *buffer, size_t offset, const unsigned char data[], size_t size);

int
dc_buffer_slice (dc_buffer_t *buffer, size_t offset, size_t size);

size_t
dc_buffer_get_size (dc_buffer_t *buffer);

unsigned char *
dc_buffer_get_data (dc_buffer_t *buffer);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BUFFER_H */
