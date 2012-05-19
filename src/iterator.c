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

#include <stddef.h>
#include <stdlib.h>

#include "iterator-private.h"

dc_status_t
dc_iterator_next (dc_iterator_t *iterator, void *item)
{
	if (iterator == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (iterator->vtable->next == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (item == NULL)
		return DC_STATUS_INVALIDARGS;

	return iterator->vtable->next (iterator, item);
}

dc_status_t
dc_iterator_free (dc_iterator_t *iterator)
{
	if (iterator == NULL)
		return DC_STATUS_SUCCESS;

	if (iterator->vtable->free == NULL)
		return DC_STATUS_UNSUPPORTED;

	return iterator->vtable->free (iterator);
}
