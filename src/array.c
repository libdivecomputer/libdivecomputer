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

void
array_reverse_nibbles (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size; ++i) {
		unsigned char tmp = data[i];
		data[i] = ((tmp & 0xF0) >> 4) | ((tmp & 0x0F) << 4);
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


int
array_convert_bin2hex (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize)
{
	if (osize != 2 * isize)
		return -1;

	const unsigned char ascii[] = {
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

	for (unsigned int i = 0; i < isize; ++i) {
		// Set the most-significant nibble.
		unsigned char msn = (input[i] >> 4) & 0x0F;
		output[i * 2 + 0] = ascii[msn];

		// Set the least-significant nibble.
		unsigned char lsn = input[i] & 0x0F;
		output[i * 2 + 1] = ascii[lsn];
	}

	return 0;
}


int
array_convert_hex2bin (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize)
{
	if (isize != 2 * osize)
		return -1;

	for (unsigned int i = 0; i < osize; ++i) {
		unsigned char value = 0;
		for (unsigned int j = 0; j < 2; ++j) {
			unsigned char number = 0;
			unsigned char ascii = input[i * 2 + j];
			if (ascii >= '0' && ascii <= '9')
				number = ascii - '0';
			else if (ascii >= 'A' && ascii <= 'F')
				number = 10 + ascii - 'A';
			else if (ascii >= 'a' && ascii <= 'f')
				number = 10 + ascii - 'a';
			else
				return -1; /* Invalid character */

			value <<= 4;
			value += number;
		}
		output[i] = value;
	}

	return 0;
}

unsigned int
array_convert_str2num (const unsigned char data[], unsigned int size)
{
	unsigned int value = 0;
	for (unsigned int i = 0; i < size; ++i) {
		if (data[i] < '0' || data[i] > '9')
			break;
		value *= 10;
		value += data[i] - '0';
	}

	return value;
}

unsigned int
array_convert_bin2dec (const unsigned char data[], unsigned int size)
{
	unsigned int value = 0;
	for (unsigned int i = 0; i < size; ++i) {
		value *= 100;
		value += data[i];
	}

	return value;
}

unsigned int
array_convert_bcd2dec (const unsigned char data[], unsigned int size)
{
	unsigned int value = 0;
	for (unsigned int i = 0; i < size; ++i) {
		value *= 100;
		value += bcd2dec(data[i]);
	}

	return value;
}

unsigned int
array_uint_be (const unsigned char data[], unsigned int n)
{
	unsigned int shift = n * 8;
	unsigned int value = 0;
	for (unsigned int i = 0; i < n; ++i) {
		shift -= 8;
		value |= (unsigned int) data[i] << shift;
	}
	return value;
}

unsigned int
array_uint_le (const unsigned char data[], unsigned int n)
{
	unsigned int shift = 0;
	unsigned int value = 0;
	for (unsigned int i = 0; i < n; ++i) {
		value |= (unsigned int) data[i] << shift;
		shift += 8;
	}
	return value;
}

unsigned long long
array_uint64_be (const unsigned char data[])
{
	return ((unsigned long long) data[0] << 56) |
	       ((unsigned long long) data[1] << 48) |
	       ((unsigned long long) data[2] << 40) |
	       ((unsigned long long) data[3] << 32) |
	       ((unsigned long long) data[4] << 24) |
	       ((unsigned long long) data[5] << 16) |
	       ((unsigned long long) data[6] <<  8) |
	       ((unsigned long long) data[7] <<  0);
}

unsigned long long
array_uint64_le (const unsigned char data[])
{
	return ((unsigned long long) data[0] <<  0) |
	       ((unsigned long long) data[1] <<  8) |
	       ((unsigned long long) data[2] << 16) |
	       ((unsigned long long) data[3] << 24) |
	       ((unsigned long long) data[4] << 32) |
	       ((unsigned long long) data[5] << 40) |
	       ((unsigned long long) data[6] << 48) |
	       ((unsigned long long) data[7] << 56);
}

unsigned int
array_uint32_be (const unsigned char data[])
{
	return ((unsigned int) data[0] << 24) |
	       ((unsigned int) data[1] << 16) |
	       ((unsigned int) data[2] <<  8) |
	       ((unsigned int) data[3] <<  0);
}


unsigned int
array_uint32_le (const unsigned char data[])
{
	return ((unsigned int) data[0] <<  0) |
	       ((unsigned int) data[1] <<  8) |
	       ((unsigned int) data[2] << 16) |
	       ((unsigned int) data[3] << 24);
}


unsigned int
array_uint32_word_be (const unsigned char data[])
{
	return ((unsigned int) data[0] <<  8) |
	       ((unsigned int) data[1] <<  0) |
	       ((unsigned int) data[2] << 24) |
	       ((unsigned int) data[3] << 16);
}

unsigned int
array_uint24_be (const unsigned char data[])
{
	return ((unsigned int) data[0] << 16) |
	       ((unsigned int) data[1] <<  8) |
	       ((unsigned int) data[2] <<  0);
}

unsigned int
array_uint24_le (const unsigned char data[])
{
	return ((unsigned int) data[0] <<  0) |
	       ((unsigned int) data[1] <<  8) |
	       ((unsigned int) data[2] << 16);
}

unsigned short
array_uint16_be (const unsigned char data[])
{
	return ((unsigned int) data[0] <<  8) |
	       ((unsigned int) data[1] <<  0);
}


unsigned short
array_uint16_le (const unsigned char data[])
{
	return ((unsigned int) data[0] <<  0) |
	       ((unsigned int) data[1] <<  8);
}

void
array_uint64_be_set (unsigned char data[], const unsigned long long input)
{
	data[0] = (input >> 56) & 0xFF;
	data[1] = (input >> 48) & 0xFF;
	data[2] = (input >> 40) & 0xFF;
	data[3] = (input >> 32) & 0xFF;
	data[4] = (input >> 24) & 0xFF;
	data[5] = (input >> 16) & 0xFF;
	data[6] = (input >>  8) & 0xFF;
	data[7] = (input      ) & 0xFF;
}

void
array_uint64_le_set (unsigned char data[], const unsigned long long input)
{
	data[0] = (input      ) & 0xFF;
	data[1] = (input >>  8) & 0xFF;
	data[2] = (input >> 16) & 0xFF;
	data[3] = (input >> 24) & 0xFF;
	data[4] = (input >> 32) & 0xFF;
	data[5] = (input >> 40) & 0xFF;
	data[6] = (input >> 48) & 0xFF;
	data[7] = (input >> 56) & 0xFF;
}

void
array_uint32_be_set (unsigned char data[], const unsigned int input)
{
	data[0] = (input >> 24) & 0xFF;
	data[1] = (input >> 16) & 0xFF;
	data[2] = (input >>  8) & 0xFF;
	data[3] = (input      ) & 0xFF;
}

void
array_uint32_le_set (unsigned char data[], const unsigned int input)
{
	data[0] = (input      ) & 0xFF;
	data[1] = (input >>  8) & 0xFF;
	data[2] = (input >> 16) & 0xFF;
	data[3] = (input >> 24) & 0xFF;
}

void
array_uint24_be_set (unsigned char data[], const unsigned int input)
{
	data[0] = (input >> 16) & 0xFF;
	data[1] = (input >>  8) & 0xFF;
	data[2] = (input      ) & 0xFF;
}

void
array_uint24_le_set (unsigned char data[], const unsigned int input)
{
	data[0] = (input      ) & 0xFF;
	data[1] = (input >>  8) & 0xFF;
	data[2] = (input >> 16) & 0xFF;
}

void
array_uint16_be_set (unsigned char data[], const unsigned short input)
{
	data[0] = (input >>  8) & 0xFF;
	data[1] = (input      ) & 0xFF;
}

void
array_uint16_le_set (unsigned char data[], const unsigned short input)
{
	data[0] = (input      ) & 0xFF;
	data[1] = (input >>  8) & 0xFF;
}

unsigned char
bcd2dec (unsigned char value)
{
	return ((value >> 4) & 0x0f) * 10 + (value & 0x0f);
}

unsigned char
dec2bcd (unsigned char value)
{
	if (value >= 100)
		return 0;

	unsigned char hi = value / 10;
	unsigned char lo = value % 10;
	return (hi << 4) | lo;
}

/*
 * When turning a two's-complement number with a certain number
 * of bits into one with more bits, the sign bit must be repeated
 * in all the extra bits.
 */
unsigned int
signextend (unsigned int value, unsigned int nbits)
{
	if (nbits <= 0 || nbits > 32)
		return 0;

	unsigned int signbit = 1U << (nbits - 1);
	unsigned int mask = signbit - 1;

	if ((value & signbit) == signbit)
		return value | ~mask;
	else
		return value & mask;
}

unsigned int
popcount (unsigned int value)
{
	unsigned int count = 0;
	while (value) {
		value &= value - 1;
		count++;
	}
	return count;
}
