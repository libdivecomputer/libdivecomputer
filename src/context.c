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

#ifdef _WIN32
#define NOGDI
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "context-private.h"

struct dc_context_t {
	dc_loglevel_t loglevel;
	dc_logfunc_t logfunc;
	void *userdata;
#ifdef ENABLE_LOGGING
	char msg[8192 + 32];
#ifdef _WIN32
	DWORD timestamp;
#else
	struct timeval timestamp;
#endif
#endif
};

#ifdef ENABLE_LOGGING
/*
 * A wrapper for the vsnprintf function, which will always null terminate the
 * string and returns a negative value if the destination buffer is too small.
 */
static int
l_vsnprintf (char *str, size_t size, const char *format, va_list ap)
{
	int n;

	if (size == 0)
		return -1;

#ifdef _MSC_VER
	/*
	 * The non-standard vsnprintf implementation provided by MSVC doesn't null
	 * terminate the string and returns a negative value if the destination
	 * buffer is too small.
	 */
	n = _vsnprintf (str, size - 1, format, ap);
	if (n == size - 1 || n < 0)
		str[size - 1] = 0;
#else
	/*
	 * The C99 vsnprintf function will always null terminate the string. If the
	 * destination buffer is too small, the return value is the number of
	 * characters that would have been written if the buffer had been large
	 * enough.
	 */
	n = vsnprintf (str, size, format, ap);
	if (n >= size)
		n = -1;
#endif

	return n;
}

static int
l_snprintf (char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int n;

	va_start (ap, format);
	n = l_vsnprintf (str, size, format, ap);
	va_end (ap);

	return n;
}

static int
l_hexdump (char *str, size_t size, const unsigned char data[], size_t n)
{
	const unsigned char ascii[] = {
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

	if (size == 0)
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

	return (n > maxlength ? -1 : length * 2);
}

static void
logfunc (dc_context_t *context, dc_loglevel_t loglevel, const char *file, unsigned int line, const char *function, const char *msg, void *userdata)
{
	const char *loglevels[] = {"NONE", "ERROR", "WARNING", "INFO", "DEBUG", "ALL"};

	unsigned long seconds = 0, milliseconds = 0;

#ifdef _WIN32
	DWORD now = GetTickCount ();
	DWORD delta = now - context->timestamp;
	seconds = delta / 1000;
	milliseconds = delta % 1000;
#else
	struct timeval now, delta;
	gettimeofday (&now, NULL);
	timersub (&now, &context->timestamp, &delta);
	seconds = delta.tv_sec;
	milliseconds = delta.tv_usec / 1000;
#endif

	if (loglevel == DC_LOGLEVEL_ERROR || loglevel == DC_LOGLEVEL_WARNING) {
		fprintf (stderr, "[%li.%03li] %s: %s [in %s:%d (%s)]\n",
			seconds, milliseconds,
			loglevels[loglevel], msg, file, line, function);
	} else {
		fprintf (stderr, "[%li.%03li] %s: %s\n",
			seconds, milliseconds,
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
	context->logfunc = logfunc;
#else
	context->loglevel = DC_LOGLEVEL_NONE;
	context->logfunc = NULL;
#endif
	context->userdata = NULL;

#ifdef ENABLE_LOGGING
	memset (context->msg, 0, sizeof (context->msg));
#ifdef _WIN32
	context->timestamp = GetTickCount ();
#else
	gettimeofday (&context->timestamp, NULL);
#endif
#endif

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
	l_vsnprintf (context->msg, sizeof (context->msg), format, ap);
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

	n = l_snprintf (context->msg, sizeof (context->msg), "%s: size=%u, data=", prefix, size);

	if (n >= 0) {
		n = l_hexdump (context->msg + n, sizeof (context->msg) - n, data, size);
	}

	context->logfunc (context, loglevel, file, line, function, context->msg, context->userdata);
#endif

	return DC_STATUS_SUCCESS;
}
