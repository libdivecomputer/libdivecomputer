#include <stdio.h>	// fopen, fwrite, fclose
#include <time.h>	// time

#include "reefnet.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


int test_dump_memory_dives (const char* name, const char* filename)
{
	sensusultra *device = NULL;
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE] = {0};

	message ("reefnet_sensusultra_open\n");
	int rc = reefnet_sensusultra_open (&device, name);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("reefnet_sensusultra_handshake\n");
	rc = reefnet_sensusultra_handshake (device, handshake, sizeof (handshake));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read handshake.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	time_t now = time (NULL);
	unsigned char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

	message ("reefnet_sensusultra_read_dives\n");
	rc = reefnet_sensusultra_read_dives (device, NULL, NULL);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read dives.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	message ("reefnet_sensusultra_close\n");
	rc = reefnet_sensusultra_close (device);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return REEFNET_SUCCESS;
}


int test_dump_memory_data (const char* name, const char* filename)
{
	sensusultra *device = NULL;
	unsigned char data[REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE] = {0};
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE] = {0};

	message ("reefnet_sensusultra_open\n");
	int rc = reefnet_sensusultra_open (&device, name);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("reefnet_sensusultra_handshake\n");
	rc = reefnet_sensusultra_handshake (device, handshake, sizeof (handshake));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read handshake.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	time_t now = time (NULL);
	unsigned char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

	message ("reefnet_sensusultra_read_data\n");
	rc = reefnet_sensusultra_read_data (device, data, sizeof (data));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read memory.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("reefnet_sensusultra_close\n");
	rc = reefnet_sensusultra_close (device);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return REEFNET_SUCCESS;
}


int test_dump_memory_user (const char* name, const char* filename)
{
	sensusultra *device = NULL;
	unsigned char data[REEFNET_SENSUSULTRA_MEMORY_USER_SIZE] = {0};
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE] = {0};

	message ("reefnet_sensusultra_open\n");
	int rc = reefnet_sensusultra_open (&device, name);
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("reefnet_sensusultra_handshake\n");
	rc = reefnet_sensusultra_handshake (device, handshake, sizeof (handshake));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read handshake.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	time_t now = time (NULL);
	unsigned char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("time=%lu (%s)\n", (unsigned long)now, datetime);

	message ("reefnet_sensusultra_read_user\n");
	rc = reefnet_sensusultra_read_user (device, data, sizeof (data));
	if (rc != REEFNET_SUCCESS) {
		WARNING ("Cannot read memory.");
		reefnet_sensusultra_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("reefnet_sensusultra_close\n");
	rc = reefnet_sensusultra_close (device);
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
	message_set_logfile ("SENSUSULTRA.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory_data (name, "SENSUSULTRA_DATA.DMP");
	int b = test_dump_memory_user (name, "SENSUSULTRA_USER.DMP");
	int c = test_dump_memory_dives (name, "SENSUSULTRA_DIVES.DMP");

	message ("SUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory_data:     %s\n", errmsg (a));
	message ("test_dump_memory_user:     %s\n", errmsg (b));
	message ("test_dump_memory_dives:    %s\n", errmsg (c));

	message_set_logfile (NULL);

	return 0;
}
