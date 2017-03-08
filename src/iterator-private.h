/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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

#ifndef DC_ITERATOR_PRIVATE_H
#define DC_ITERATOR_PRIVATE_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/iterator.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_iterator_vtable_t dc_iterator_vtable_t;

struct dc_iterator_t {
	const dc_iterator_vtable_t *vtable;
	dc_context_t *context;
};

struct dc_iterator_vtable_t {
	size_t size;
	dc_status_t (*next) (dc_iterator_t *iterator, void *item);
	dc_status_t (*free) (dc_iterator_t *iterator);
};

dc_iterator_t *
dc_iterator_allocate (dc_context_t *context, const dc_iterator_vtable_t *vtable);

void
dc_iterator_deallocate (dc_iterator_t *iterator);

int
dc_iterator_isinstance (dc_iterator_t *iterator, const dc_iterator_vtable_t *vtable);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_ITERATOR_PRIVATE_H */
