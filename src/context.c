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

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "context-private.h"

struct dc_context_t {
	dc_loglevel_t loglevel;
};

dc_status_t
dc_context_new (dc_context_t **out)
{
	dc_context_t *context = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	context = (dc_context_t *) malloc (sizeof (dc_context_t));
	if (context == NULL)
		return DC_STATUS_NOMEMORY;

	context->loglevel = DC_LOGLEVEL_WARNING;

	*out = context;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_free (dc_context_t *context)
{
	free (context);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_set_loglevel (dc_context_t *context, dc_loglevel_t loglevel)
{
	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

	context->loglevel = loglevel;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_log (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *format, ...)
{
	va_list ap;

	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

	if (loglevel > context->loglevel)
		return DC_STATUS_SUCCESS;

	va_start (ap, format);
	vfprintf (stderr, format, ap);
	va_end (ap);

	return DC_STATUS_SUCCESS;
}
