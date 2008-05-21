#include <assert.h>
#include <string.h>

#include "suunto_common.h"

#define SUBSTRACT(a, x, begin, end) substract (a - begin, x, end - begin) + begin
#define DISTANCE(a, b, begin, end) distance (a, b, end - begin)

static unsigned int
substract (unsigned int a, unsigned int x, unsigned int size)
{
	if (x <= a) {
		return (a - x) % size;
	} else {
		return size - (x - a) % size;
	}
}

static unsigned int
distance (unsigned int a, unsigned int b, unsigned int size)
{
	if (a <= b) {
		return (b - a) % size;
	} else {
		return size - (a - b) % size;
	}
}

int
suunto_common_extract_dives (const unsigned char data[], unsigned int begin, unsigned int end, unsigned int eop, unsigned int peek, dive_callback_t callback, void *userdata)
{
	assert (eop >= begin && eop < end);
	assert (data[eop] == 0x82);

	unsigned char buffer[0x2000 - 0x4C] = {0};
	assert (sizeof (buffer) >= end - begin);

	unsigned int current = eop;
	unsigned int previous = eop;
	for (unsigned int i = 0; i < end - begin; ++i) {
		// Move backwards through the ringbuffer.
		if (current == begin)
			current = end;
		current--;

		// Check for an end of profile marker.
		if (data[current] == 0x82)
			break;

		// Check for an end of dive marker (of the next dive),
		// to find the start of the current dive.
		unsigned int peek = SUBSTRACT (current, peek, begin, end);
		if (data[peek] == 0x80) {
			unsigned int len = DISTANCE (current, previous, begin, end);
			if (current + len > end) {
				unsigned int a = end - current;
				unsigned int b = (current + len) - end;
				memcpy (buffer + 0, data + current, a);
				memcpy (buffer + a, data + begin,   b);
			} else {
				memcpy (buffer, data + current, len);
			}

			if (callback)
				callback (buffer, len, userdata);

			previous = current;
		}
	}

	assert (data[current] == 0x82);

	return SUUNTO_SUCCESS;
}
