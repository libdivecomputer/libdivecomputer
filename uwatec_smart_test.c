#include <stdio.h>	// fopen, fwrite, fclose
#include <stdlib.h>	// malloc, free
#include <string.h>	// memset

#include "uwatec.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


int test_dump_memory (const char* filename)
{
	smart *device = NULL;

	const unsigned int size = 2 * 1024 * 1024;
	unsigned char *data = malloc (size * sizeof (unsigned char));
	if (data == NULL)
		return UWATEC_ERROR_MEMORY;
	memset (data, 0, size * sizeof (unsigned char));

	message ("uwatec_smart_open\n");
	int rc = uwatec_smart_open (&device);
	if (rc != UWATEC_SUCCESS) {
		WARNING ("Cannot open device.");
		free (data);
		return rc;
	}

	message ("uwatec_smart_read\n");
	rc = uwatec_smart_read (device, data, size);
	if (rc != UWATEC_SUCCESS) {
		WARNING ("Cannot read data.");
		uwatec_smart_close (device);
		free (data);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), size, fp);
		fclose (fp);
	}

	message ("uwatec_smart_close\n");
	rc = uwatec_smart_close (device);
	if (rc != UWATEC_SUCCESS) {
		WARNING ("Cannot close device.");
		free (data);
		return rc;
	}

	free (data);

	return UWATEC_SUCCESS;
}


const char* errmsg (int rc)
{
	switch (rc) {
	case UWATEC_SUCCESS:
		return "Success";
	case UWATEC_ERROR:
		return "Generic error";
	case UWATEC_ERROR_IO:
		return "Input/output error";
	case UWATEC_ERROR_MEMORY:
		return "Memory error";
	case UWATEC_ERROR_PROTOCOL:
		return "Protocol error";
	case UWATEC_ERROR_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}


int main(int argc, char *argv[])
{
	message_set_logfile ("SMART.LOG");

	int a = test_dump_memory ("SMART.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory:          %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
