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

#include <libdivecomputer/descriptor.h>

struct dc_descriptor_t {
	const char *vendor;
	const char *product;
	dc_family_t type;
	unsigned int model;
};

const char *
dc_descriptor_get_vendor (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return NULL;

	return descriptor->vendor;
}

const char *
dc_descriptor_get_product (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return NULL;

	return descriptor->product;
}

dc_family_t
dc_descriptor_get_type (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return DC_FAMILY_NULL;

	return descriptor->type;
}

unsigned int
dc_descriptor_get_model (dc_descriptor_t *descriptor)
{
	if (descriptor == NULL)
		return 0;

	return descriptor->model;
}
