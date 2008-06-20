#include <stdio.h>
#include <stdarg.h>

static FILE* g_logfile = NULL;

int message (const char* fmt, ...)
{
	va_list ap;
	
	if (g_logfile) {
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
}
