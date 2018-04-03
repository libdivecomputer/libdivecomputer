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

#ifndef DC_CONTEXT_H
#define DC_CONTEXT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_context_t dc_context_t;

typedef enum dc_loglevel_t {
	DC_LOGLEVEL_NONE,
	DC_LOGLEVEL_ERROR,
	DC_LOGLEVEL_WARNING,
	DC_LOGLEVEL_INFO,
	DC_LOGLEVEL_DEBUG,
	DC_LOGLEVEL_ALL
} dc_loglevel_t;

typedef void (*dc_logfunc_t) (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *message, void *userdata);

dc_status_t
dc_context_new (dc_context_t **context);

dc_status_t
dc_context_free (dc_context_t *context);

dc_status_t
dc_context_set_loglevel (dc_context_t *context, dc_loglevel_t loglevel);

dc_status_t
dc_context_set_logfunc (dc_context_t *context, dc_logfunc_t logfunc, void *userdata);

unsigned int
dc_context_get_transports (dc_context_t *context);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_CONTEXT_H */
