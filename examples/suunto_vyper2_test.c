#include <stdio.h>	// fopen, fwrite, fclose

#include "suunto.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

int test_dump_sdm (const char* name)
{
	vyper2 *device = NULL;

	message ("suunto_vyper2_open\n");
	int rc = suunto_vyper2_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_vyper2_read_version\n");
	unsigned char version[SUUNTO_VYPER2_VERSION_SIZE] = {0};
	rc = suunto_vyper2_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("suunto_vyper2_read_dives\n");
	rc = suunto_vyper2_read_dives (device, NULL, NULL);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read dives.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("suunto_vyper2_close\n");
	rc = suunto_vyper2_close (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return SUUNTO_SUCCESS;
}

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER2_MEMORY_SIZE] = {0};
	vyper2 *device = NULL;

	message ("suunto_vyper2_open\n");
	int rc = suunto_vyper2_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_vyper2_read_version\n");
	unsigned char version[SUUNTO_VYPER2_VERSION_SIZE] = {0};
	rc = suunto_vyper2_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("suunto_vyper2_read_memory\n");
	rc = suunto_vyper2_read_memory (device, 0x00, data, sizeof (data));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("suunto_vyper2_close\n");
	rc = suunto_vyper2_close (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot close device.");
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
	message_set_logfile ("VYPER2.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory (name, "VYPER2.DMP");
	int b = test_dump_sdm (name);

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory: %s\n", errmsg (a));
	message ("test_dump_sdm:    %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
