/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <stdlib.h> // malloc, realloc, free
#include <string.h> // memcpy, memmove

#include <libdivecomputer/buffer.h>

struct dc_buffer_t {
	unsigned char *data;
	size_t capacity, offset, size;
};

dc_buffer_t *
dc_buffer_new (size_t capacity)
{
	dc_buffer_t *buffer = (dc_buffer_t *) malloc (sizeof (dc_buffer_t));
	if (buffer == NULL)
		return NULL;

	if (capacity) {
		buffer->data = (unsigned char *) malloc (capacity);
		if (buffer->data == NULL) {
			free (buffer);
			return NULL;
		}
	} else {
		buffer->data = NULL;
	}

	buffer->capacity = capacity;
	buffer->offset = 0;
	buffer->size = 0;

	return buffer;
}


void
dc_buffer_free (dc_buffer_t *buffer)
{
	if (buffer == NULL)
		return;

	if (buffer->data)
		free (buffer->data);

	free (buffer);
}


int
dc_buffer_clear (dc_buffer_t *buffer)
{
	if (buffer == NULL)
		return 0;

	buffer->offset = 0;
	buffer->size = 0;

	return 1;
}


static size_t
dc_buffer_expand_calc (dc_buffer_t *buffer, size_t n)
{
	size_t oldsize = buffer->capacity;
	size_t newsize = (oldsize ? oldsize : n);
	while (newsize < n)
		newsize *= 2;

	return newsize;
}


static int
dc_buffer_expand_append (dc_buffer_t *buffer, size_t n)
{
	if (n > buffer->capacity - buffer->offset) {
		if (n > buffer->capacity) {
			size_t capacity = dc_buffer_expand_calc (buffer, n);

			unsigned char *data = (unsigned char *) malloc (capacity);
			if (data == NULL)
				return 0;

			if (buffer->size)
				memcpy (data, buffer->data + buffer->offset, buffer->size);

			free (buffer->data);

			buffer->data = data;
			buffer->capacity = capacity;
			buffer->offset = 0;
		} else {
			if (buffer->size)
				memmove (buffer->data, buffer->data + buffer->offset, buffer->size);

			buffer->offset = 0;
		}
	}

	return 1;
}


static int
dc_buffer_expand_prepend (dc_buffer_t *buffer, size_t n)
{
	size_t available = buffer->capacity - buffer->size;

	if (n > buffer->offset + buffer->size) {
		if (n > buffer->capacity) {
			size_t capacity = dc_buffer_expand_calc (buffer, n);

			unsigned char *data = (unsigned char *) malloc (capacity);
			if (data == NULL)
				return 0;

			if (buffer->size)
				memcpy (data + capacity - buffer->size, buffer->data + buffer->offset, buffer->size);

			free (buffer->data);

			buffer->data = data;
			buffer->capacity = capacity;
			buffer->offset = capacity - buffer->size;
		} else {
			if (buffer->size)
				memmove (buffer->data + available, buffer->data + buffer->offset, buffer->size);

			buffer->offset = available;
		}
	}

	return 1;
}


int
dc_buffer_reserve (dc_buffer_t *buffer, size_t capacity)
{
	if (buffer == NULL)
		return 0;

	if (capacity <= buffer->capacity)
		return 1;

	unsigned char *data = (unsigned char *) realloc (buffer->data, capacity);
	if (data == NULL)
		return 0;

	buffer->data = data;
	buffer->capacity = capacity;

	return 1;
}


int
dc_buffer_resize (dc_buffer_t *buffer, size_t size)
{
	if (buffer == NULL)
		return 0;

	if (!dc_buffer_expand_append (buffer, size))
		return 0;

	if (size > buffer->size)
		memset (buffer->data + buffer->offset + buffer->size, 0, size - buffer->size);

	buffer->size = size;

	return 1;
}


int
dc_buffer_append (dc_buffer_t *buffer, const unsigned char data[], size_t size)
{
	if (buffer == NULL)
		return 0;

	if (!dc_buffer_expand_append (buffer, buffer->size + size))
		return 0;

	if (size)
		memcpy (buffer->data + buffer->offset + buffer->size, data, size);

	buffer->size += size;

	return 1;
}


int
dc_buffer_prepend (dc_buffer_t *buffer, const unsigned char data[], size_t size)
{
	if (buffer == NULL)
		return 0;

	if (!dc_buffer_expand_prepend (buffer, buffer->size + size))
		return 0;

	if (size)
		memcpy (buffer->data + buffer->offset - size, data, size);

	buffer->size += size;
	buffer->offset -= size;

	return 1;
}


int
dc_buffer_insert (dc_buffer_t *buffer, size_t offset, const unsigned char data[], size_t size)
{
	if (buffer == NULL)
		return 0;

	if (offset > buffer->size)
		return 0;

	size_t head = buffer->offset;
	size_t tail = buffer->capacity - (buffer->offset + buffer->size);

	unsigned char *ptr = buffer->data + buffer->offset;

	if (size <= head) {
		if (buffer->size)
			memmove (ptr - size, ptr, offset);
		buffer->offset -= size;
	} else if (size <= tail) {
		if (buffer->size)
			memmove (ptr + offset + size, ptr + offset, buffer->size - offset);
	} else if (size <= tail + head) {
		size_t n = buffer->size + size;
		size_t available = buffer->capacity - n;

		size_t tmp_offset = head > tail ? available : 0;

		unsigned char *tmp = buffer->data;

		if (buffer->size) {
			memmove (tmp + tmp_offset, ptr, offset);
			memmove (tmp + tmp_offset + offset + size, ptr + offset, buffer->size - offset);
		}

		buffer->offset = tmp_offset;
	} else {
		size_t n = buffer->size + size;
		size_t capacity = dc_buffer_expand_calc (buffer, n);
		size_t available = capacity - n;

		size_t tmp_offset = head > tail ? available : 0;

		unsigned char *tmp = (unsigned char *) malloc (capacity);
		if (tmp == NULL)
			return 0;

		if (buffer->size) {
			memcpy (tmp + tmp_offset, ptr, offset);
			memcpy (tmp + tmp_offset + offset + size, ptr + offset, buffer->size - offset);
		}

		free (buffer->data);
		buffer->data = tmp;
		buffer->capacity = capacity;
		buffer->offset = tmp_offset;
	}

	if (size)
		memcpy (buffer->data + buffer->offset + offset, data, size);

	buffer->size += size;

	return 1;
}


int
dc_buffer_slice (dc_buffer_t *buffer, size_t offset, size_t size)
{
	if (buffer == NULL)
		return 0;

	if (offset + size > buffer->size)
		return 0;

	buffer->offset += offset;
	buffer->size = size;

	return 1;
}


size_t
dc_buffer_get_size (dc_buffer_t *buffer)
{
	if (buffer == NULL)
		return 0;

	return buffer->size;
}


unsigned char *
dc_buffer_get_data (dc_buffer_t *buffer)
{
	if (buffer == NULL)
		return NULL;

	return buffer->size ? buffer->data + buffer->offset : NULL;
}
