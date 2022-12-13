/*
 * libdivecomputer
 *
 * Copyright (C) 2022 Jef Driesen
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stddef.h>

#include "oceans_s1_common.h"

int
oceans_s1_getline (char **line, size_t *linelen, const unsigned char **data, size_t *size)
{
	if (line == NULL || linelen == NULL || data == NULL || size == NULL)
		return -1;

	if (*size == 0)
		return -1;

	// Find the end of the line.
	unsigned int strip = 0;
	const unsigned char *p = *data, *end = p + *size;
	while (p != end) {
		unsigned char c = *p++;
		if (c == '\r' || c == '\n') {
			strip = 1;
			break;
		}
	}

	// Get the length of the line.
	size_t len = p - *data;

	// Resize the buffer (if necessary).
	if (*line == NULL || len + 1 > *linelen) {
		char *buf = (char *) malloc (len + 1);
		if (buf == NULL)
			return -1;
		free (*line);
		*line = buf;
		*linelen = len + 1;
	}

	// Copy the data.
	memcpy (*line, *data, len - strip);
	(*line)[len - strip] = 0;
	*data += len;
	*size -= len;

	return len - strip;
}
