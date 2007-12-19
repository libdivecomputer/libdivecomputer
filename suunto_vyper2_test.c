#include <stdio.h>	// fprintf

#include "suunto.h"

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER2_MEMORY_SIZE] = {0};
	vyper2 *device = NULL;

	printf ("suunto_vyper2_open\n");
	int rc = suunto_vyper2_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		fprintf (stderr, "Error opening serial port.\n");
		return rc;
	}

	printf ("suunto_vyper2_read_version\n");
	unsigned char version[4] = {0};
	rc = suunto_vyper2_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		fprintf (stderr, "Cannot identify computer.\n");
		suunto_vyper2_close (device);
		return rc;
	}

	printf ("suunto_vyper2_read_memory\n");
	rc = suunto_vyper2_read_memory (device, 0x00, data, sizeof (data));
	if (rc != SUUNTO_SUCCESS) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper2_close (device);
		return rc;
	}

	printf ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	printf ("suunto_vyper2_close\n");
	rc = suunto_vyper2_close (device);
	if (rc != SUUNTO_SUCCESS) {
		fprintf (stderr, "Cannot close device.");
		return rc;
	}

	return SUUNTO_SUCCESS;
}

const char* errmsg (int rc)
{
	switch (rc) {
	case SUUNTO_SUCCESS:
		return "Success";
	case SUUNTO_ERROR:
		return "Generic error";
	case SUUNTO_ERROR_IO:
		return "Input/output error";
	case SUUNTO_ERROR_MEMORY:
		return "Memory error";
	case SUUNTO_ERROR_PROTOCOL:
		return "Protocol error";
	case SUUNTO_ERROR_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	int a = test_dump_memory (name, "VYPER2.DMP");

	printf ("\nSUMMARY\n");
	printf ("-------\n");
	printf ("test_dump_memory: %s\n", errmsg (a));

	return 0;
}
