/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h>

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


int
array_isequal (const unsigned char data[], unsigned int size, unsigned char value)
{
	for (unsigned int i = 0; i < size; ++i) {
		if (data[i] != value)
			return 0;
	}

	return 1;
}


const unsigned char *
array_search_forward (const unsigned char *data, unsigned int size,
                      const unsigned char *marker, unsigned int msize)
{
	while (size >= msize) {
		if (memcmp (data, marker, msize) == 0)
			return data;
		size--;
		data++;
	}
	return NULL;
}


const unsigned char *
array_search_backward (const unsigned char *data, unsigned int size,
                       const unsigned char *marker, unsigned int msize)
{
	data += size;
	while (size >= msize) {
		if (memcmp (data - msize, marker, msize) == 0)
			return data;
		size--;
		data--;
	}
	return NULL;
}


unsigned int
array_uint32_be (const unsigned char data[])
{
	return (data[0] << 24) + (data[1] << 16) + (data[2] << 8) + data[3];
}


unsigned int
array_uint32_le (const unsigned char data[])
{
	return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}


unsigned int
array_uint24_be (const unsigned char data[])
{
	return (data[0] << 16) + (data[1] << 8) + data[2];
}


unsigned int
array_uint24_le (const unsigned char data[])
{
	return data[0] + (data[1] << 8) + (data[2] << 16);
}

unsigned short
array_uint16_be (const unsigned char data[])
{
	return (data[0] << 8) + data[1];
}


unsigned short
array_uint16_le (const unsigned char data[])
{
	return data[0] + (data[1] << 8);
}

unsigned char
bcd2dec (unsigned char value)
{
	return ((value >> 4) & 0x0f) * 10 + (value & 0x0f);
}
