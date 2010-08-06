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

int
irda_errcode (void)
{
	return 0;
}


const char *
irda_errmsg (void)
{
	return NULL;
}


int
irda_init (void)
{
	return -1;
}


int
irda_cleanup (void)
{
	return -1;
}


int
irda_socket_open (irda_t **out)
{
    return -1;
}


int
irda_socket_close (irda_t *device)
{
	return -1;
}


int
irda_socket_set_timeout (irda_t *device, long timeout)
{
	return -1;
}


int
irda_socket_discover (irda_t *device, irda_callback_t callback, void *userdata)
{
	return -1;
}


int
irda_socket_connect_name (irda_t *device, unsigned int address, const char *name)
{
	return -1;
}


int
irda_socket_connect_lsap (irda_t *device, unsigned int address, unsigned int lsap)
{
	return -1;
}


int
irda_socket_available (irda_t *device)
{
	return -1;
}


int
irda_socket_read (irda_t *device, void *data, unsigned int size)
{
	return -1;
}


int
irda_socket_write (irda_t *device, const void *data, unsigned int size)
{
	return -1;
}
