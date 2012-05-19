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

#include <libdivecomputer/iterator.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_iterator_vtable_t dc_iterator_vtable_t;

struct dc_iterator_t {
	const dc_iterator_vtable_t *vtable;
};

struct dc_iterator_vtable_t {
	dc_status_t (*free) (dc_iterator_t *iterator);
	dc_status_t (*next) (dc_iterator_t *iterator, void *item);
};

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_ITERATOR_PRIVATE_H */
