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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#include "context-private.h"
#include "platform.h"
#include "timer.h"

struct dc_context_t {
	dc_loglevel_t loglevel;
	dc_logfunc_t logfunc;
	void *userdata;
#ifdef ENABLE_LOGGING
	char msg[16384 + 32];
	dc_timer_t *timer;
#endif
};

#ifdef ENABLE_LOGGING
static int
l_hexdump (char *str, size_t size, const unsigned char data[], size_t n)
{
	const unsigned char ascii[] = {
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

	if (size == 0 || size > INT_MAX)
		return -1;

	/* The maximum number of bytes. */
	size_t maxlength = (size - 1) / 2;

	/* The actual number of bytes. */
	size_t length = (n > maxlength ? maxlength : n);

	for (size_t i = 0; i < length; ++i) {
		/* Set the most-significant nibble. */
		unsigned char msn = (data[i] >> 4) & 0x0F;
		str[i * 2 + 0] = ascii[msn];

		/* Set the least-significant nibble. */
		unsigned char lsn = data[i] & 0x0F;
		str[i * 2 + 1] = ascii[lsn];
	}

	/* Null terminate the hex string. */
	str[length * 2] = 0;

	return (n > maxlength ? -1 : (int) (length * 2));
}

static void
loghandler (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *msg, void *userdata)
{
	const char *loglevels[] = {"NONE", "ERROR", "WARNING", "INFO", "DEBUG", "ALL"};

	dc_usecs_t now = 0;
	dc_timer_now (context->timer, &now);

	unsigned long seconds = now / 1000000;
	unsigned long microseconds = now % 1000000;

	if (loglevel == DC_LOGLEVEL_ERROR || loglevel == DC_LOGLEVEL_WARNING) {
		fprintf (stderr, "[%li.%06li] %s: %s [in %s:%d (%s)]\n",
			seconds, microseconds,
			loglevels[loglevel], msg, file, line, function);
	} else {
		fprintf (stderr, "[%li.%06li] %s: %s\n",
			seconds, microseconds,
			loglevels[loglevel], msg);
	}
}
#endif

dc_status_t
dc_context_new (dc_context_t **out)
{
	dc_context_t *context = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	context = (dc_context_t *) malloc (sizeof (dc_context_t));
	if (context == NULL)
		return DC_STATUS_NOMEMORY;

#ifdef ENABLE_LOGGING
	context->loglevel = DC_LOGLEVEL_WARNING;
	context->logfunc = loghandler;
#else
	context->loglevel = DC_LOGLEVEL_NONE;
	context->logfunc = NULL;
#endif
	context->userdata = NULL;

#ifdef ENABLE_LOGGING
	memset (context->msg, 0, sizeof (context->msg));
	context->timer = NULL;
	dc_timer_new (&context->timer);
#endif

	*out = context;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_free (dc_context_t *context)
{
	if (context == NULL)
		return DC_STATUS_SUCCESS;

#ifdef ENABLE_LOGGING
	dc_timer_free (context->timer);
#endif
	free (context);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_set_loglevel (dc_context_t *context, dc_loglevel_t loglevel)
{
	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef ENABLE_LOGGING
	context->loglevel = loglevel;
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_set_logfunc (dc_context_t *context, dc_logfunc_t logfunc, void *userdata)
{
	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef ENABLE_LOGGING
	context->logfunc = logfunc;
	context->userdata = userdata;
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_log (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *format, ...)
{
#ifdef ENABLE_LOGGING
	va_list ap;
#endif

	if (context == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef ENABLE_LOGGING
	if (loglevel > context->loglevel)
		return DC_STATUS_SUCCESS;

	if (context->logfunc == NULL)
		return DC_STATUS_SUCCESS;

	va_start (ap, format);
	dc_platform_vsnprintf (context->msg, sizeof (context->msg), format, ap);
	va_end (ap);

	context->logfunc (context, loglevel, file, line, function, context->msg, context->userdata);
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_context_syserror (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, int errcode)
{
	const char *errmsg = NULL;

#ifdef _WIN32
	char buffer[256];

	DWORD rc = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, errcode, 0, buffer, sizeof (buffer), NULL);

	/* Remove the CRLF and period at the end of the error message. */
	while (rc > 0 && (
		buffer[rc - 1] == '\n' ||
		buffer[rc - 1] == '\r' ||
		buffer[rc - 1] == '.'))
	{
		buffer[rc - 1] = '\0';
		rc--;
	}

	if (rc > 0)
		errmsg = buffer;
#elif defined (HAVE_STRERROR_R)
	char buffer[256];

	int rc = strerror_r (errcode, buffer, sizeof (buffer));
	if (rc == 0)
		errmsg = buffer;
#else
	/* Fallback to the non-threadsafe function. */
	errmsg = strerror (errcode);
#endif

	if (errmsg == NULL)
		errmsg = "Unknown system error";

	return dc_context_log (context, loglevel, file, line, function, "%s (%d)", errmsg, errcode);
}

dc_status_t
dc_context_hexdump (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *prefix, const unsigned char data[], unsigned int size)
{
#ifdef ENABLE_LOGGING
	int n;
#endif

	if (context == NULL || prefix == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef ENABLE_LOGGING
	if (loglevel > context->loglevel)
		return DC_STATUS_SUCCESS;

	if (context->logfunc == NULL)
		return DC_STATUS_SUCCESS;

	n = dc_platform_snprintf (context->msg, sizeof (context->msg), "%s: size=%u, data=", prefix, size);

	if (n >= 0) {
		n = l_hexdump (context->msg + n, sizeof (context->msg) - n, data, size);
	}

	context->logfunc (context, loglevel, file, line, function, context->msg, context->userdata);
#endif

	return DC_STATUS_SUCCESS;
}

unsigned int
dc_context_get_transports (dc_context_t *context)
{
	UNUSED(context);

	return DC_TRANSPORT_SERIAL
#if defined(HAVE_LIBUSB)
		| DC_TRANSPORT_USB
#endif
#if defined(HAVE_HIDAPI)
		| DC_TRANSPORT_USBHID
#elif defined(HAVE_LIBUSB) && !defined(__APPLE__)
		| DC_TRANSPORT_USBHID
#endif
#ifdef _WIN32
#ifdef HAVE_AF_IRDA_H
		| DC_TRANSPORT_IRDA
#endif
#ifdef HAVE_WS2BTH_H
		| DC_TRANSPORT_BLUETOOTH
#endif
#else /* _WIN32 */
#ifdef HAVE_LINUX_IRDA_H
		| DC_TRANSPORT_IRDA
#endif
#ifdef HAVE_BLUEZ
		| DC_TRANSPORT_BLUETOOTH
#endif
#endif /* _WIN32 */
	;
}
