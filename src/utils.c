#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "utils.h"

static FILE* g_logfile = NULL;

static unsigned char g_lastchar = '\n';

#ifdef _WIN32
	#include <windows.h>
	static unsigned long g_timestamp;
#else
	#include <sys/time.h>
	static struct timeval g_timestamp;
#endif

int message (const char* fmt, ...)
{
	va_list ap;

	if (g_logfile) {
		if (g_lastchar == '\n') {
#ifdef _WIN32
			unsigned long timestamp = GetTickCount () - g_timestamp;
			unsigned long sec = timestamp / 1000L, msec = timestamp % 1000L;
			fprintf (g_logfile, "[%li.%03li] ", sec, msec);
#else
			struct timeval now = {0}, timestamp = {0};
			gettimeofday (&now, NULL);
			timersub (&now, &g_timestamp, &timestamp);
			fprintf (g_logfile, "[%lli.%06lli] ", (long long)timestamp.tv_sec, (long long)timestamp.tv_usec);
#endif
		}

		size_t len = strlen (fmt);
		if (len > 0)
			g_lastchar = fmt[len - 1];
		else
			g_lastchar = 0;

		va_start (ap, fmt);
		vfprintf (g_logfile, fmt, ap);
		va_end (ap);
	}

	va_start (ap, fmt);
	int rc = vfprintf (stdout, fmt, ap);
	va_end (ap);

	return rc;
}

void message_set_logfile (const char* filename)
{
	if (g_logfile) {
		fclose (g_logfile);
		g_logfile = NULL;
	}

	if (filename)
		g_logfile = fopen (filename, "w");

	if (g_logfile) {
		g_lastchar = '\n';
#ifdef _WIN32
		g_timestamp = GetTickCount ();
#else
		gettimeofday (&g_timestamp, NULL);
#endif
	}
}
