#include <stdio.h>	// fopen, fwrite, fclose
#include <time.h>	// time

#include "reefnet.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

int test_dump_memory (const char* name, const char* filename)
{
	sensuspro *device = NULL;
	unsigned char data[REEFNET_SENSUSPRO_MEMORY_SIZE] = {0};
	unsigned char handshake[REEFNET_SENSUSPRO_HANDSHAKE_SIZE] = {0};

	message ("reefnet_sensuspro_open\n");
	int rc = reefnet_sensuspro_open (&device, name);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("reefnet_sensuspro_handshake\n");
	rc = reefnet_sensuspro_handshake (device, handshake, sizeof (handshake));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read handshake.");
		reefnet_sensuspro_close (device);
		return rc;
	}

	time_t now = time (NULL);
	char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

	message ("reefnet_sensuspro_read\n");
	rc = reefnet_sensuspro_read (device, data, sizeof (data));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read memory.");
		reefnet_sensuspro_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("reefnet_sensuspro_close\n");
	rc = reefnet_sensuspro_close (device);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return REEFNET_SUCCESS;
}


const char* errmsg (int rc)
{
	switch (rc) {
	case REEFNET_SUCCESS:
		return "Success";
	case REEFNET_ERROR:
		return "Generic error";
	case REEFNET_ERROR_IO:
		return "Input/output error";
	case REEFNET_ERROR_MEMORY:
		return "Memory error";
	case REEFNET_ERROR_PROTOCOL:
		return "Protocol error";
	case REEFNET_ERROR_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}


int main(int argc, char *argv[])
{
	message_set_logfile ("SENSUSPRO.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory (name, "SENSUSPRO.DMP");

	message ("SUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory:          %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
