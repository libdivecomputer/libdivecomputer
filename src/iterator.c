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
#include <assert.h>

#include "context-private.h"
#include "iterator-private.h"

dc_iterator_t *
dc_iterator_allocate (dc_context_t *context, const dc_iterator_vtable_t *vtable)
{
	dc_iterator_t *iterator = NULL;

	assert(vtable != NULL);
	assert(vtable->size >= sizeof(dc_iterator_t));

	// Allocate memory.
	iterator = (dc_iterator_t *) malloc (vtable->size);
	if (iterator == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return iterator;
	}

	iterator->vtable = vtable;
	iterator->context = context;

	return iterator;
}

void
dc_iterator_deallocate (dc_iterator_t *iterator)
{
	free (iterator);
}

int
dc_iterator_isinstance (dc_iterator_t *iterator, const dc_iterator_vtable_t *vtable)
{
	if (iterator == NULL)
		return 0;

	return iterator->vtable == vtable;
}

dc_status_t
dc_iterator_next (dc_iterator_t *iterator, void *item)
{
	if (iterator == NULL || iterator->vtable->next == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (item == NULL)
		return DC_STATUS_INVALIDARGS;

	return iterator->vtable->next (iterator, item);
}

dc_status_t
dc_iterator_free (dc_iterator_t *iterator)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (iterator == NULL)
		return DC_STATUS_SUCCESS;

	if (iterator->vtable->free) {
		status = iterator->vtable->free (iterator);
	}

	dc_iterator_deallocate (iterator);

	return status;
}
