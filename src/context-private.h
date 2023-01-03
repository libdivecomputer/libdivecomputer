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

#ifndef DC_CONTEXT_PRIVATE_H
#define DC_CONTEXT_PRIVATE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libdivecomputer/context.h>

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define UNUSED(x) (void)sizeof(x)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define FUNCTION __func__
#else
#define FUNCTION __FUNCTION__
#endif

#ifdef ENABLE_LOGGING
#define HEXDUMP(context, loglevel, prefix, data, size) dc_context_hexdump (context, loglevel, __FILE__, __LINE__, FUNCTION, prefix, data, size)
#define SYSERROR(context, errcode) dc_context_syserror (context, DC_LOGLEVEL_ERROR, __FILE__, __LINE__, FUNCTION, errcode)
#define ERROR(context, ...) dc_context_log (context, DC_LOGLEVEL_ERROR, __FILE__, __LINE__, FUNCTION, __VA_ARGS__)
#define WARNING(context, ...) dc_context_log (context, DC_LOGLEVEL_WARNING, __FILE__, __LINE__, FUNCTION, __VA_ARGS__)
#define INFO(context, ...) dc_context_log (context, DC_LOGLEVEL_INFO, __FILE__, __LINE__, FUNCTION, __VA_ARGS__)
#define DEBUG(context, ...) dc_context_log (context, DC_LOGLEVEL_DEBUG, __FILE__, __LINE__, FUNCTION, __VA_ARGS__)
#else
#define HEXDUMP(context, loglevel, prefix, data, size) UNUSED(context)
#define SYSERROR(context, errcode) UNUSED(context)
#define ERROR(context, ...) UNUSED(context)
#define WARNING(context, ...) UNUSED(context)
#define INFO(context, ...) UNUSED(context)
#define DEBUG(context, ...) UNUSED(context)
#endif

dc_status_t
dc_context_log (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *format, ...) DC_ATTR_FORMAT_PRINTF(6, 7);

dc_status_t
dc_context_syserror (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, int errcode);

dc_status_t
dc_context_hexdump (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *prefix, const unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_CONTEXT_PRIVATE_H */
