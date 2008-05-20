#include <stdio.h>	// fopen, fwrite, fclose

#include "uwatec.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


int test_dump_memory (const char* name, const char* filename)
{
	memomouse *device = NULL;
	unsigned char data[0x8000] = {0};

	message ("uwatec_memomouse_open\n");
	int rc = uwatec_memomouse_open (&device, name);
	if (rc != UWATEC_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("uwatec_memomouse_read\n");
	rc = uwatec_memomouse_read (device, data, sizeof (data));
	if (rc < 0) {
		WARNING ("Cannot read memory.");
		uwatec_memomouse_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), rc, fp);
		fclose (fp);
	}

	message ("uwatec_memomouse_close\n");
	rc = uwatec_memomouse_close (device);
	if (rc != UWATEC_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

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
	message_set_logfile ("MEMOMOUSE.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory (name, "MEMOMOUSE.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory:          %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
