#include <stdio.h>     // fopen, fwrite, fclose

#include "suunto.h"
#include "serial.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

int test_dump_sdm16 (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE] = {0};
	vyper *device = NULL;

	message ("suunto_vyper_open\n");
	int rc = suunto_vyper_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_vyper_detect_interface\n");
	rc = suunto_vyper_detect_interface (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Interface not found.");
		suunto_vyper_close (device);
		return rc;
	}

	message ("suunto_vyper_read_memory\n");
	rc = suunto_vyper_read_memory (device, 0x24, data + 0x24, 1);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x1E, data + 0x1E, 14);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x2C, data + 0x2C, 32);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x53, data + 0x53, 30);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper_close (device);
		return rc;
	}

	message ("suunto_vyper_read_dive\n");
	int ndives = 0;
	int offset = 0x71;
	do {
		message ("Reading dive #%d.\n", ndives + 1);
		rc = suunto_vyper_read_dive (device, data + offset, sizeof (data) - offset, (ndives == 0));
		if (rc < 0) {
			WARNING ("Cannot read dive.");
			suunto_vyper_close (device);
			return rc;
		}
		message ("Returned %i bytes at offset 0x%04x.\n", rc, offset);
		ndives++;
		offset += rc;
	} while (rc > 0);

	message ("Dumping data\n");
	FILE *fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("suunto_vyper_close\n");
	rc = suunto_vyper_close (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return SUUNTO_SUCCESS;
}

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE] = {0};
	vyper *device = NULL;

	message ("suunto_vyper_open\n");
	int rc = suunto_vyper_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_vyper_detect_interface\n");
	rc = suunto_vyper_detect_interface (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Interface not found.");
		suunto_vyper_close (device);
		return rc;
	}

	message ("suunto_vyper_read_memory\n");
	rc = suunto_vyper_read_memory (device, 0x00, data, sizeof (data));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("suunto_vyper_close\n");
	rc = suunto_vyper_close (device);
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
	message_set_logfile ("VYPER.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	int a = test_dump_sdm16 (name, "VYPER.SDM");
	int b = test_dump_memory (name, "VYPER.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_sdm16:  %s\n", errmsg (a));
	message ("test_dump_memory: %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
