/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#include <stdlib.h>
#include <assert.h>

#include "output-private.h"

dctool_output_t *
dctool_output_allocate (const dctool_output_vtable_t *vtable)
{
	dctool_output_t *output = NULL;

	assert(vtable != NULL);
	assert(vtable->size >= sizeof(dctool_output_t));

	// Allocate memory.
	output = (dctool_output_t *) malloc (vtable->size);
	if (output == NULL) {
		return output;
	}

	output->vtable = vtable;
	output->number = 0;

	return output;
}

void
dctool_output_deallocate (dctool_output_t *output)
{
	free (output);
}

dc_status_t
dctool_output_write (dctool_output_t *output, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize)
{
	if (output == NULL || output->vtable->write == NULL)
		return DC_STATUS_SUCCESS;

	output->number++;

	return output->vtable->write (output, parser, data, size, fingerprint, fsize);
}

dc_status_t
dctool_output_free (dctool_output_t *output)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (output == NULL)
		return DC_STATUS_SUCCESS;

	if (output->vtable->free) {
		status = output->vtable->free (output);
	}

	dctool_output_deallocate (output);

	return status;
}
