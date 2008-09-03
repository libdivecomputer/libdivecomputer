#include <stdio.h>	// fopen, fwrite, fclose

#include "suunto_d9.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

device_status_t
test_dump_sdm (const char* name)
{
	device_t *device = NULL;

	message ("suunto_d9_device_open\n");
	device_status_t rc = suunto_d9_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("device_version\n");
	unsigned char version[SUUNTO_D9_VERSION_SIZE] = {0};
	rc = device_version (device, version, sizeof (version));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot identify computer.");
		device_close (device);
		return rc;
	}

	message ("device_foreach\n");
	rc = device_foreach (device, NULL, NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read dives.");
		device_close (device);
		return rc;
	}

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_D9_MEMORY_SIZE] = {0};
	device_t *device = NULL;

	message ("suunto_d9_device_open\n");
	device_status_t rc = suunto_d9_device_open (&device, name);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("device_version\n");
	unsigned char version[SUUNTO_D9_VERSION_SIZE] = {0};
	rc = device_version (device, version, sizeof (version));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot identify computer.");
		device_close (device);
		return rc;
	}

	message ("device_read\n");
	rc = device_read (device, 0x00, data, sizeof (data));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory.");
		device_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return DEVICE_STATUS_SUCCESS;
}


const char*
errmsg (device_status_t rc)
{
	switch (rc) {
	case DEVICE_STATUS_SUCCESS:
		return "Success";
	case DEVICE_STATUS_UNSUPPORTED:
		return "Unsupported operation";
	case DEVICE_STATUS_TYPE_MISMATCH:
		return "Device type mismatch";
	case DEVICE_STATUS_ERROR:
		return "Generic error";
	case DEVICE_STATUS_IO:
		return "Input/output error";
	case DEVICE_STATUS_MEMORY:
		return "Memory error";
	case DEVICE_STATUS_PROTOCOL:
		return "Protocol error";
	case DEVICE_STATUS_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}

int main(int argc, char *argv[])
{
	message_set_logfile ("D9.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	device_status_t a = test_dump_memory (name, "D9.DMP");
	device_status_t b = test_dump_sdm (name);

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory: %s\n", errmsg (a));
	message ("test_dump_sdm:    %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
