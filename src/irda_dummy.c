/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
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

#include "irda.h"

dc_status_t
dc_irda_open (dc_irda_t **out, dc_context_t *context)
{
    return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_close (dc_irda_t *device)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_set_timeout (dc_irda_t *device, int timeout)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_discover (dc_irda_t *device, dc_irda_callback_t callback, void *userdata)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_connect_name (dc_irda_t *device, unsigned int address, const char *name)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_connect_lsap (dc_irda_t *device, unsigned int address, unsigned int lsap)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_get_available (dc_irda_t *device, size_t *value)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_read (dc_irda_t *device, void *data, size_t size, size_t *actual)
{
	return DC_STATUS_UNSUPPORTED;
}

dc_status_t
dc_irda_write (dc_irda_t *device, const void *data, size_t size, size_t *actual)
{
	return DC_STATUS_UNSUPPORTED;
}
