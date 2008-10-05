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
