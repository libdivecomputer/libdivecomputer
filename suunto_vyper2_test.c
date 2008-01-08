#include <stdio.h>	// fopen, fwrite, fclose
#include <assert.h>	// assert
#include <string.h>	// memcpy, memmove

#include "suunto.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define DISTANCE(a,b) distance (a, b, SUUNTO_VYPER2_MEMORY_SIZE - 0x019A)

unsigned int
distance (unsigned int a, unsigned int b, unsigned int size)
{
	if (a <= b) {
		return (b - a) % size;
	} else {
		return size - (a - b) % size;
	}
}

int test_dump_sdm (const char* name, const char* filename)
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
	unsigned char version[4] = {0};
	rc = suunto_vyper2_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("suunto_vyper2_read_memory\n");
	rc = suunto_vyper2_read_memory (device, 0x00, data + 0x00, SUUNTO_VYPER2_PACKET_SIZE);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper2_close (device);
		return rc;
	}
	rc = suunto_vyper2_read_memory (device, 0x168, data + 0x168, SUUNTO_VYPER2_PACKET_SIZE);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper2_close (device);
		return rc;
	}
	rc = suunto_vyper2_read_memory (device, 0xF0, data + 0xF0, SUUNTO_VYPER2_PACKET_SIZE);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_vyper2_close (device);
		return rc;
	}

	message ("suunto_vyper2_read_dive\n");
	unsigned int last = data[0x0190] + (data[0x0191] << 8);
	unsigned int count = data[0x0192] + (data[0x0193] << 8);
	unsigned int end = data[0x0194] + (data[0x0195] << 8);
	unsigned int begin = data[0x0196] + (data[0x0197] << 8);
	message ("Pointers: begin=%04x, last=%04x, end=%04x, count=%i\n", begin, last, end, count);

	unsigned int index = 0x019A;

	// Calculate the total amount of bytes.

	unsigned int remaining = DISTANCE (begin, end);

	// To reduce the number of read operations, we always try to read 
	// packages with the largest possible size. As a consequence, the 
	// last package of a dive can contain data from more than one dive. 
	// Therefore, the remaining data of this package (and its size) 
	// needs to be preserved for the next dive.
	// The package buffer also needs some extra space to store the 
	// pointer bytes (see later on for the reason).

	unsigned int available = 0;
	unsigned char package[SUUNTO_VYPER2_PACKET_SIZE + 3] = {0};

	// The ring buffer is traversed backwards to retrieve the most recent
	// dives first. This allows you to download only the new dives. During 
	// the traversal, the current pointer does always point to the end of
	// the dive data and we move to the "next" dive by means of the previous 
	// pointer.

	unsigned int ndives = 0;
	unsigned int current = end;
	unsigned int previous = last;
	while (current != begin) {
		// Calculate the size of the current dive.
		unsigned int size = DISTANCE (previous, current);
		message ("Pointers: dive=%u, current=%04x, previous=%04x, size=%u, remaining=%u\n", ndives + 1, current, previous, size, remaining);
		assert (size >= 4);

		// Check if the output buffer is large enough to store the entire 
		// dive. The output buffer does not need space for the previous and 
		// next pointers (4 bytes).
		unsigned char *pointer = data + index + size - 4;
		if (sizeof (data) - index < size - 4) {
			WARNING ("Insufficient buffer space available.");
			return SUUNTO_ERROR_MEMORY;
		}

		// If there is already some data available from downloading 
		// the previous dive, it is processed here.
		if (available) {
			// Calculate the offset to the package data.
			unsigned int offset = 0;
			if (available > size - 4)
				offset = available - (size - 4);

			// Prepend the package data to the output buffer.
			pointer -= available - offset;
			memcpy (pointer, package + offset, available - offset);
		}

		unsigned int nbytes = available;
		unsigned int address = current - available;
		while (nbytes < size) {
			// Calculate the package size. Try with the largest possible 
			// size first, and adjust when the end of the ringbuffer or  
			// the end of the profile data is reached.
			unsigned int len = SUUNTO_VYPER2_PACKET_SIZE;
			if (0x019A + len > address)
				len = address - 0x019A; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.
			/*if (nbytes + len > size)
				len = size - nbytes;*/ // End of dive (for testing only).

			// Calculate the offset to the package data, skipping the
			// previous and next pointers (4 bytes) and also the data
			// from the next dive (if present). Thus, the offset is only 
			// non-zero for packages at the end of the dive.
			unsigned int offset = 0;
			if (nbytes + len > size - 4)
				offset = nbytes + len - (size - 4);

			message ("Pointers: address=%04x, len=%u, offset=%u\n", address - len, len, offset);

			// The previous and next pointers can be split over multiple 
			// packages. If that is the case, we move those byte(s) from 
			// the start of the previous package to the end of the current 
			// package. With this approach, the pointers are preserved when 
			// reading the current package (in the next step) and they are 
			// again in a continuous memory area.
			if (offset > len) {
				message ("Pointers: dst=%u, src=%u, len=%u\n", len, 0, offset - len);
				memmove (package + len, package, offset - len);
			}

			// Read the package.
			rc = suunto_vyper2_read_memory (device, address - len, package, len);
			if (rc != SUUNTO_SUCCESS) {
				WARNING ("Cannot read memory.");
				suunto_vyper2_close (device);
				return rc;
			}

			// Prepend the package data to the output buffer.
			if (offset < len) {
				pointer -= len - offset;
				memcpy (pointer, package + offset, len - offset);
			}

			// Next package.
			nbytes += len;
			address -= len;
			if (address <= 0x019A)
				address = SUUNTO_VYPER2_MEMORY_SIZE;		
		}

		// The last package of the current dive contains the previous and
		// next pointers (in a continuous memory area). It can also contain
		// a number of bytes from the next dive. The offset to the pointers
		// is equal to the number of remaining (or "available") bytes.

		available = nbytes - size;
		message ("Pointers: nbytes=%u, available=%u\n", nbytes, available);

		unsigned int oprevious = package[available + 0x00] + (package[available + 0x01] << 8);
		unsigned int onext     = package[available + 0x02] + (package[available + 0x03] << 8);
		message ("Pointers: previous=%04x, next=%04x\n", oprevious, onext);
		assert (current == onext);

		// Next dive.
		current = previous;
		previous = oprevious;
		remaining -= size;
		ndives++;

#ifndef NDEBUG
		message ("Vyper2Profile()=\"");
		for (unsigned int i = 0; i < size - 4; ++i) {
			message ("%02x", data[i + index]);
		}
		message ("\"\n");
#endif

		index += size - 4;
	}
	assert (remaining == 0);
	assert (available == 0);

	message ("Dumping data\n");
	FILE *fp = fopen (filename, "wb");
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
	unsigned char version[4] = {0};
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

	int a = test_dump_memory (name, "VYPER2.DMP");
	int b = test_dump_sdm (name, "VYPER2.SDM");

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory: %s\n", errmsg (a));
	message ("test_dump_sdm:    %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
