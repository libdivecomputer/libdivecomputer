#include <stdio.h>	// fopen, fwrite, fclose
#include <stdlib.h>	// malloc, free
#include <string.h>	// memset

#include "uwatec_smart.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


device_status_t
test_dump_memory (const char* filename)
{
	device_t *device = NULL;

	const unsigned int size = 2 * 1024 * 1024;
	unsigned char *data = malloc (size * sizeof (unsigned char));
	if (data == NULL)
		return DEVICE_STATUS_MEMORY;
	memset (data, 0, size * sizeof (unsigned char));

	message ("uwatec_smart_device_open\n");
	device_status_t rc = uwatec_smart_device_open (&device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot open device.");
		free (data);
		return rc;
	}

	message ("uwatec_smart_device_handshake\n");
	rc = uwatec_smart_device_handshake (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Handshake failed.");
		device_close (device);
		free (data);
		return rc;
	}

	message ("uwatec_smart_device_version\n");
	unsigned char version[UWATEC_SMART_VERSION_SIZE] = {0};
	rc = uwatec_smart_device_version (device, version, sizeof (version));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot identify computer.");
		device_close (device);
		free (data);
		return rc;
	}

	message ("device_dump\n");
	unsigned int nbytes = 0;
	rc = device_dump (device, data, size, &nbytes);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read data.");
		device_close (device);
		free (data);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), nbytes, fp);
		fclose (fp);
	}

	message ("device_close\n");
	rc = device_close (device);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot close device.");
		free (data);
		return rc;
	}

	free (data);

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
	message_set_logfile ("SMART.LOG");

	device_status_t a = test_dump_memory ("SMART.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory:          %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
