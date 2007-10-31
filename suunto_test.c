#include <stdio.h>	// fprintf

#include "suunto.h"
#include "serial.h"

int main(int argc, char *argv[])
{
	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE] = {0};

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	vyper *device = NULL;

	printf ("suunto_vyper_open\n");
	int rc = suunto_vyper_open (&device, name);
	if (rc != 0) {
		fprintf (stderr, "%s:%d: Error opening serial port.\n",__FILE__,__LINE__);
		return 1;
	}

	printf ("suunto_vyper_detect_interface\n");
	rc = suunto_vyper_detect_interface (device);
	if (rc != 0) {
		fprintf (stderr, "%s:%d: Interface not found.\n",__FILE__,__LINE__);
		suunto_vyper_close (device);
		return 1;
	}

	printf ("suunto_vyper_read_memory\n");
	rc = suunto_vyper_read_memory (device, 0x24, data, 1);
	if (rc != 0) {
		fprintf (stderr, "%s:%d: Cannot identify computer.\n",__FILE__,__LINE__);
		suunto_vyper_close (device);
		return 1;
	}
	rc = suunto_vyper_read_memory (device, 0x1E, data, 14);
	if (rc != 0) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return 1;
	}
	rc = suunto_vyper_read_memory (device, 0x2C, data, 32);
	if (rc != 0) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return 1;
	}
	rc = suunto_vyper_read_memory (device, 0x53, data, 30);
	if (rc != 0) {
		fprintf (stderr, "Cannot read memory.\n");
		suunto_vyper_close (device);
		return 1;
	}

	printf ("suunto_vyper_read_dive\n");
	int ndives = 0;
	do {
		fprintf (stderr, "Reading dive #%d.\n", ndives + 1);
		rc = suunto_vyper_read_dive (device, data, sizeof (data), (ndives == 0));
		if (rc == -1) {
			fprintf (stderr, "Cannot read dive.\n");
			suunto_vyper_close (device);
			return 1;
		}
		ndives++;
	} while (rc > 0);

	printf ("suunto_vyper_close\n");
	rc = suunto_vyper_close (device);
	if (rc != 0) {
		fprintf (stderr, "Cannot close device.");
		return 1;
	}

	return 0;
}
