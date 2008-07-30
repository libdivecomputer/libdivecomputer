#include "array.h"

void
array_reverse_bytes (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size / 2; ++i) {
		unsigned char hlp = data[i];
		data[i] = data[size - 1 - i];
		data[size - 1 - i] = hlp;
	}
}


void
array_reverse_bits (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size; ++i) {
		unsigned char j = 0;
		j  = (data[i] & 0x01) << 7;
		j += (data[i] & 0x02) << 5;
		j += (data[i] & 0x04) << 3;
		j += (data[i] & 0x08) << 1;
		j += (data[i] & 0x10) >> 1;
		j += (data[i] & 0x20) >> 3;
		j += (data[i] & 0x40) >> 5;
		j += (data[i] & 0x80) >> 7;
		data[i] = j;
	}
}
