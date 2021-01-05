/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "output-private.h"
#include "utils.h"

static dc_status_t dctool_raw_output_write (dctool_output_t *output, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize);
static dc_status_t dctool_raw_output_free (dctool_output_t *output);

typedef struct dctool_raw_output_t {
	dctool_output_t base;
	char *template;
} dctool_raw_output_t;

static const dctool_output_vtable_t raw_vtable = {
	sizeof(dctool_raw_output_t), /* size */
	dctool_raw_output_write, /* write */
	dctool_raw_output_free, /* free */
};

static int
mktemplate_fingerprint (char *buffer, size_t size, const unsigned char fingerprint[], size_t fsize)
{
	const unsigned char ascii[] = {
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

	if (size < 2 * fsize + 1)
		return -1;

	for (size_t i = 0; i < fsize; ++i) {
		// Set the most-significant nibble.
		unsigned char msn = (fingerprint[i] >> 4) & 0x0F;
		buffer[i * 2 + 0] = ascii[msn];

		// Set the least-significant nibble.
		unsigned char lsn = fingerprint[i] & 0x0F;
		buffer[i * 2 + 1] = ascii[lsn];
	}

	// Null-terminate the string.
	buffer[fsize * 2] = 0;

	return fsize * 2;
}

static int
mktemplate_datetime (char *buffer, size_t size, dc_parser_t *parser)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_datetime_t datetime = {0};
	int n = 0;

	rc = dc_parser_get_datetime (parser, &datetime);
	if (rc != DC_STATUS_SUCCESS)
		return -1;

	n = snprintf (buffer, size, "%04i%02i%02iT%02i%02i%02i",
		datetime.year, datetime.month, datetime.day,
		datetime.hour, datetime.minute, datetime.second);
	if (n < 0 || (size_t) n >= size)
		return -1;

	return n;
}

static int
mktemplate_number (char *buffer, size_t size, unsigned int number)
{
	int n = 0;

	n = snprintf (buffer, size, "%04u", number);
	if (n < 0 || (size_t) n >= size)
		return -1;

	return n;
}

static int
mktemplate (char *buffer, size_t size, const char *format, dc_parser_t *parser, const unsigned char fingerprint[], size_t fsize, unsigned int number)
{
	const char *p = format;
	size_t n = 0;
	int len = 0;
	char ch = 0;

	while ((ch = *p++) != 0) {
		if (ch != '%') {
			if (n >= size)
				return -1;
			buffer[n] = ch;
			n++;
			continue;
		}

		ch = *p++;
		switch (ch) {
		case '%':
			if (n >= size)
				return -1;
			buffer[n] = ch;
			n++;
			break;
		case 't': // Timestamp
			len = mktemplate_datetime (buffer + n, size - n, parser);
			if (len < 0)
				return -1;
			n += len;
			break;
		case 'f': // Fingerprint
			len = mktemplate_fingerprint (buffer + n, size - n, fingerprint, fsize);
			if (len < 0)
				return -1;
			n += len;
			break;
		case 'n': // Number
			len = mktemplate_number (buffer + n, size - n, number);
			if (len < 0)
				return -1;
			n += len;
			break;
		default:
			return -1;
		}
	}

	// Null-terminate the string
	if (n >= size)
		return -1;
	buffer[n] = 0;

	return n;
}

dctool_output_t *
dctool_raw_output_new (const char *template)
{
	dctool_raw_output_t *output = NULL;

	if (template == NULL)
		goto error_exit;

	// Allocate memory.
	output = (dctool_raw_output_t *) dctool_output_allocate (&raw_vtable);
	if (output == NULL) {
		goto error_exit;
	}

	output->template = strdup(template);
	if (output->template == NULL) {
		goto error_free;
	}

	return (dctool_output_t *) output;

error_free:
	dctool_output_deallocate ((dctool_output_t *) output);
error_exit:
	return NULL;
}

static dc_status_t
dctool_raw_output_write (dctool_output_t *abstract, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize)
{
	dctool_raw_output_t *output = (dctool_raw_output_t *) abstract;

	// Generate the filename.
	char name[1024] = {0};
	int ret = mktemplate (name, sizeof(name), output->template, parser, fingerprint, fsize, abstract->number);
	if (ret < 0) {
		ERROR("Failed to generate filename from template.");
		return DC_STATUS_SUCCESS;
	}

	// Open the output file.
	FILE *fp = fopen (name, "wb");
	if (fp == NULL) {
		ERROR("Failed to open the output file.");
		return DC_STATUS_SUCCESS;
	}

	// Write the data.
	fwrite (data, sizeof (unsigned char), size, fp);
	fclose (fp);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dctool_raw_output_free (dctool_output_t *abstract)
{
	dctool_raw_output_t *output = (dctool_raw_output_t *) abstract;

	free (output->template);

	return DC_STATUS_SUCCESS;
}
