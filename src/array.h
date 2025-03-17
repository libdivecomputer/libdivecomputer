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

#ifndef ARRAY_H
#define ARRAY_H

#define C_ARRAY_SIZE(a) (sizeof (a) / sizeof *(a))
#define C_ARRAY_ITEMSIZE(a) (sizeof *(a))

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void
array_reverse_bytes (unsigned char data[], unsigned int size);

void
array_reverse_bits (unsigned char data[], unsigned int size);

void
array_reverse_nibbles (unsigned char data[], unsigned int size);

int
array_isequal (const unsigned char data[], unsigned int size, unsigned char value);

const unsigned char *
array_search_forward (const unsigned char *data, unsigned int size,
                      const unsigned char *marker, unsigned int msize);

const unsigned char *
array_search_backward (const unsigned char *data, unsigned int size,
                       const unsigned char *marker, unsigned int msize);

int
array_convert_bin2hex (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize);

int
array_convert_hex2bin (const unsigned char input[], unsigned int isize, unsigned char output[], unsigned int osize);

unsigned int
array_convert_str2num (const unsigned char data[], unsigned int size);

unsigned int
array_convert_bin2dec (const unsigned char data[], unsigned int size);

unsigned int
array_convert_bcd2dec (const unsigned char data[], unsigned int size);

unsigned int
array_uint_be (const unsigned char data[], unsigned int n);

unsigned int
array_uint_le (const unsigned char data[], unsigned int n);

unsigned long long
array_uint64_be (const unsigned char data[]);

unsigned long long
array_uint64_le (const unsigned char data[]);

unsigned int
array_uint32_be (const unsigned char data[]);

unsigned int
array_uint32_le (const unsigned char data[]);

unsigned int
array_uint32_word_be (const unsigned char data[]);

unsigned int
array_uint24_be (const unsigned char data[]);

unsigned int
array_uint24_le (const unsigned char data[]);

unsigned short
array_uint16_be (const unsigned char data[]);

unsigned short
array_uint16_le (const unsigned char data[]);

void
array_uint64_be_set (unsigned char data[], const unsigned long long input);

void
array_uint64_le_set (unsigned char data[], const unsigned long long input);

void
array_uint32_be_set (unsigned char data[], const unsigned int input);

void
array_uint32_le_set (unsigned char data[], const unsigned int input);

void
array_uint24_be_set (unsigned char data[], const unsigned int input);

void
array_uint24_le_set (unsigned char data[], const unsigned int input);

void
array_uint16_be_set (unsigned char data[], const unsigned short input);

void
array_uint16_le_set (unsigned char data[], const unsigned short input);

unsigned char
bcd2dec (unsigned char value);

unsigned char
dec2bcd (unsigned char value);

unsigned int
signextend (unsigned int value, unsigned int nbits);

unsigned int
popcount (unsigned int value);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* ARRAY_H */
