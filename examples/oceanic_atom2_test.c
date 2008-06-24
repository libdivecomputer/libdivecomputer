#include <stdio.h>	// fopen, fwrite, fclose

#include "oceanic.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[OCEANIC_ATOM2_MEMORY_SIZE] = {0};
	atom2 *device = NULL;

	message ("oceanic_atom2_open\n");
	int rc = oceanic_atom2_open (&device, name);
	if (rc != OCEANIC_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("oceanic_atom2_read_version\n");
	unsigned char version[OCEANIC_ATOM2_PACKET_SIZE] = {0};
	rc = oceanic_atom2_read_version (device, version, sizeof (version));
	if (rc != OCEANIC_SUCCESS) {
		WARNING ("Cannot identify computer.");
		oceanic_atom2_close (device);
		return rc;
	}

	message ("oceanic_atom2_read_memory\n");
	rc = oceanic_atom2_read_memory (device, 0x00, data, sizeof (data));
	if (rc != OCEANIC_SUCCESS) {
		WARNING ("Cannot read memory.");
		oceanic_atom2_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("oceanic_atom2_read_dives\n");
	rc = oceanic_atom2_read_dives (device, NULL, NULL);
	if (rc != OCEANIC_SUCCESS) {
		WARNING ("Cannot read dives.");
		oceanic_atom2_close (device);
		return rc;
	}

	message ("oceanic_atom2_close\n");
	rc = oceanic_atom2_close (device);
	if (rc != OCEANIC_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return OCEANIC_SUCCESS;
}

const char* errmsg (int rc)
{
	switch (rc) {
	case OCEANIC_SUCCESS:
		return "Success";
	case OCEANIC_ERROR:
		return "Generic error";
	case OCEANIC_ERROR_IO:
		return "Input/output error";
	case OCEANIC_ERROR_MEMORY:
		return "Memory error";
	case OCEANIC_ERROR_PROTOCOL:
		return "Protocol error";
	case OCEANIC_ERROR_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}

int main(int argc, char *argv[])
{
	message_set_logfile ("ATOM2.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory (name, "ATOM2.DMP");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory: %s\n", errmsg (a));

	message_set_logfile (NULL);

	return 0;
}
