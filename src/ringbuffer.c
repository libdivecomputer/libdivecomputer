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
modulo (unsigned int x, unsigned int n, unsigned int d)
{
	unsigned int result = 0;
	if (d > x) {
#if 0
		result = (n - (d - x) % n) % n;
#else
		unsigned int m = (d - x) % n;
		result = m ? n - m : m;
#endif
	} else {
		result = (x - d) % n;
	}

	return result + d;
}


static unsigned int
distance (unsigned int a, unsigned int b, unsigned int n, unsigned int mode)
{
	unsigned int result = 0;
	if (a > b) {
#if 0
		result = (n - (a - b) % n) % n;
#else
		unsigned int m = (a - b) % n;
		result = m ? n - m : m;
#endif
	} else {
		result = (b - a) % n;
	}

	if (result == 0) {
		return (mode == 0 ? 0 : n);
	} else {
		return result;
	}
}


unsigned int
ringbuffer_normalize (unsigned int a, unsigned int begin, unsigned int end)
{
	assert (end > begin);

	unsigned int n = end - begin;
	return modulo (a, n, begin);
}


unsigned int
ringbuffer_distance (unsigned int a, unsigned int b, int mode, unsigned int begin, unsigned int end)
{
	assert (end > begin);

	unsigned int n = end - begin;
	return distance (a, b, n, mode);
}


unsigned int
ringbuffer_increment (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end > begin);

	unsigned int n = end - begin;
	return modulo (a + delta % n, n, begin);
}


unsigned int
ringbuffer_decrement (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end > begin);

	unsigned int n = end - begin;
	return modulo (a + n - delta % n, n, begin);
}
