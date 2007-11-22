#include <stdio.h>	// fprintf

#include "suunto.h"
#include "serial.h"


int test_dump_sdm16 (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE] = {0};
	vyper *device = NULL;

	printf ("suunto_vyper_open\n");
	int rc = suunto_vyper_open (&device, name);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "%s:%d: Error opening serial port.\n",__FILE__,__LINE__);
		return rc;
	}

	printf ("suunto_vyper_detect_interface\n");
	rc = suunto_vyper_detect_interface (device);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "%s:%d: Interface not found.\n",__FILE__,__LINE__);
		suunto_vyper_close (device);
		return rc;
	}

	printf ("suunto_vyper_read_memory\n");
	rc = suunto_vyper_read_memory (device, 0x24, data + 0x24, 1);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "%s:%d: Cannot identify computer.\n",__FILE__,__LINE__);
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x1E, data + 0x1E, 14);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x2C, data + 0x2C, 32);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return rc;
	}
	rc = suunto_vyper_read_memory (device, 0x53, data + 0x53, 30);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return rc;
	}

	printf ("suunto_vyper_read_dive\n");
	int ndives = 0;
	int offset = 0x71;
	do {
		fprintf (stderr, "Reading dive #%d.\n", ndives + 1);
		rc = suunto_vyper_read_dive (device, data + offset, sizeof (data) - offset, (ndives == 0));
		if (rc < 0) {
			fprintf (stderr, "Cannot read dive.\n");
			suunto_vyper_close (device);
			return rc;
		}
		fprintf (stderr, "Returned %i bytes at offset 0x%04x.\n", rc, offset);
		ndives++;
		offset += rc;
	} while (rc > 0);

	printf ("Dumping data\n");
	FILE *fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	printf ("suunto_vyper_close\n");
	rc = suunto_vyper_close (device);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot close device.");
		return rc;
	}

	return SUUNTO_VYPER_SUCCESS;
}

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE] = {0};
	vyper *device = NULL;

	printf ("suunto_vyper_open\n");
	int rc = suunto_vyper_open (&device, name);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "%s:%d: Error opening serial port.\n",__FILE__,__LINE__);
		return rc;
	}

	printf ("suunto_vyper_detect_interface\n");
	rc = suunto_vyper_detect_interface (device);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "%s:%d: Interface not found.\n",__FILE__,__LINE__);
		suunto_vyper_close (device);
		return rc;
	}

	printf ("suunto_vyper_read_memory\n");
	rc = suunto_vyper_read_memory (device, 0x00, data, sizeof (data));
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return rc;
	}

	printf ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	printf ("suunto_vyper_close\n");
	rc = suunto_vyper_close (device);
	if (rc != SUUNTO_VYPER_SUCCESS) {
		fprintf (stderr, "Cannot close device.");
		return rc;
	}

	return SUUNTO_VYPER_SUCCESS;
}

const char* errmsg (int rc)
{
	switch (rc) {
	case SUUNTO_VYPER_SUCCESS:
		return "Success";
	case SUUNTO_VYPER_ERROR:
		return "Generic error";
	case SUUNTO_VYPER_ERROR_IO:
		return "Input/output error";
	case SUUNTO_VYPER_ERROR_MEMORY:
		return "Memory error";
	case SUUNTO_VYPER_ERROR_PROTOCOL:
		return "Protocol error";
	case SUUNTO_VYPER_ERROR_TIMEOUT:
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

	int a = test_dump_sdm16 (name, "PROFILE.SDM");
	int b = test_dump_memory (name, "PROFILE.DMP");

	printf ("\nSUMMARY\n");
	printf ("-------\n");
	printf ("test_dump_sdm16:  %s\n", errmsg (a));
	printf ("test_dump_memory: %s\n", errmsg (b));

	return 0;
}
