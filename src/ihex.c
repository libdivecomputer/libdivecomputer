/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ihex.h"
#include "context-private.h"
#include "checksum.h"
#include "array.h"

struct dc_ihex_file_t {
	dc_context_t *context;
	FILE *fp;
};

dc_status_t
dc_ihex_file_open (dc_ihex_file_t **result, dc_context_t *context, const char *filename)
{
	dc_ihex_file_t *file = NULL;

	if (result == NULL || filename == NULL) {
		ERROR (context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	file = (dc_ihex_file_t *) malloc (sizeof (dc_ihex_file_t));
	if (file == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	file->context = context;

	file->fp = fopen (filename, "rb");
	if (file->fp == NULL) {
		ERROR (context, "Failed to open the file.");
		free (file);
		return DC_STATUS_IO;
	}

	*result = file;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_ihex_file_read (dc_ihex_file_t *file, dc_ihex_entry_t *entry)
{
	unsigned char ascii[9 + 2 * 255 + 2] = {0};
	unsigned char data[4 + 255 + 1] = {0};
	unsigned int type, length, address;
	unsigned char csum_a, csum_b;
	size_t n;

	if (file == NULL || entry == NULL) {
		ERROR (file ? file->context : NULL, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	/* Read the start code. */
	while (1) {
		n = fread (ascii, 1, 1, file->fp);
		if (n != 1) {
			if (feof (file->fp)) {
				return DC_STATUS_DONE;
			} else {
				ERROR (file->context, "Failed to read the start code.");
				return DC_STATUS_IO;
			}
		}

		if (ascii[0] == ':')
			break;

		/* Ignore CR and LF characters. */
		if (ascii[0] != '\n' && ascii[0] != '\r') {
			ERROR (file->context, "Unexpected character (0x%02x).", ascii[0]);
			return DC_STATUS_DATAFORMAT;
		}
	}

	/* Read the record length, address and type. */
	n = fread (ascii + 1, 1, 8, file->fp);
	if (n != 8) {
		ERROR (file->context, "Failed to read the header.");
		return DC_STATUS_IO;
	}

	/* Convert to binary representation. */
	if (array_convert_hex2bin (ascii + 1, 8, data, 4) != 0) {
		ERROR (file->context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	/* Get the record length. */
	length = data[0];

	/* Read the record payload. */
	n = fread (ascii + 9, 1, 2 * length + 2, file->fp);
	if (n != 2 * length + 2) {
		ERROR (file->context, "Failed to read the data.");
		return DC_STATUS_IO;
	}

	/* Convert to binary representation. */
	if (array_convert_hex2bin (ascii + 9, 2 * length + 2, data + 4, length + 1) != 0) {
		ERROR (file->context, "Invalid hexadecimal character.");
		return DC_STATUS_DATAFORMAT;
	}

	/* Verify the checksum. */
	csum_a = data[4 + length];
	csum_b = ~checksum_add_uint8 (data, 4 + length, 0x00) + 1;
	if (csum_a != csum_b) {
		ERROR (file->context, "Unexpected checksum (0x%02x, 0x%02x).", csum_a, csum_b);
		return DC_STATUS_DATAFORMAT;
	}

	/* Get the record address. */
	address = array_uint16_be (data + 1);

	/* Get the record type. */
	type = data[3];
	if (type < 0 || type > 5) {
		ERROR (file->context, "Invalid record type (0x%02x).", type);
		return DC_STATUS_DATAFORMAT;
	}

	/* Verify the length and address. */
	if (type != 0) {
		unsigned int len = 0;
		switch (type) {
		case 1: /* End of file record. */
			len = 0;
			break;
		case 2: /* Extended segment address record. */
		case 4: /* Extended linear address record. */
			len = 2;
			break;
		case 3: /* Start segment address record. */
		case 5: /* Start linear address record. */
			len = 4;
			break;
		}
		if (length != len || address != 0) {
			ERROR (file->context, "Invalid record length or address.");
			return DC_STATUS_DATAFORMAT;
		}
	}

	/* Set the record fields. */
	entry->type = type;
	entry->address = address;
	entry->length = length;

	/* Copy the record data. */
	memcpy (entry->data, data + 4, entry->length);
	memset (entry->data + entry->length, 0, sizeof (entry->data) - entry->length);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_ihex_file_reset (dc_ihex_file_t *file)
{
	if (file == NULL) {
		ERROR (NULL, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	rewind (file->fp);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_ihex_file_close (dc_ihex_file_t *file)
{
	if (file) {
		fclose (file->fp);
		free (file);
	}

	return DC_STATUS_SUCCESS;
}
