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
	dc_logfunc_t logfunc;
	void *userdata;
	char msg[4096];
};

static void
logfunc (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *msg, void *userdata)
{
	const char *loglevels[] = {"NONE", "ERROR", "WARNING", "INFO", "DEBUG", "ALL"};

	fprintf (stderr, "%s: %s [in %s:%d (%s)]\n", loglevels[loglevel], msg, file, line, function);
}

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
	context->logfunc = logfunc;
	context->userdata = NULL;

	memset (context->msg, 0, sizeof (context->msg));

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
dc_context_set_logfunc (dc_context_t *context, dc_logfunc_t logfunc, void *userdata)
{
	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

	context->logfunc = logfunc;
	context->userdata = userdata;

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

	if (context->logfunc == NULL)
		return DC_STATUS_SUCCESS;

	va_start (ap, format);
	vsnprintf (context->msg, sizeof (context->msg), format, ap);
	va_end (ap);

	context->logfunc (context, loglevel, file, line, function, context->msg, context->userdata);

	return DC_STATUS_SUCCESS;
}
