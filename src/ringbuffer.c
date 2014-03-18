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

#include <assert.h>

#include "ringbuffer.h"


static unsigned int
normalize (unsigned int a, unsigned int size)
{
	return a % size;
}


static unsigned int
distance (unsigned int a, unsigned int b, int mode, unsigned int size)
{
	if (a < b) {
		return (b - a) % size;
	} else if (a > b) {
		return size - (a - b) % size;
	} else {
		return (mode == 0 ? 0 : size);
	}
}


static unsigned int
increment (unsigned int a, unsigned int delta, unsigned int size)
{
	return (a + delta) % size;
}


static unsigned int
decrement (unsigned int a, unsigned int delta, unsigned int size)
{
	if (delta <= a) {
		return (a - delta) % size;
	} else {
		return size - (delta - a) % size;
	}
}


unsigned int
ringbuffer_normalize (unsigned int a, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return normalize (a, end - begin);
}


unsigned int
ringbuffer_distance (unsigned int a, unsigned int b, int mode, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return distance (a, b, mode, end - begin);
}


unsigned int
ringbuffer_increment (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return increment (a - begin, delta, end - begin) + begin;
}


unsigned int
ringbuffer_decrement (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return decrement (a - begin, delta, end - begin) + begin;
}
