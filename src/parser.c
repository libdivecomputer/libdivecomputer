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

#include <stdlib.h>

#include "parser-private.h"


void
parser_init (parser_t *parser, const parser_backend_t *backend)
{
	parser->backend = backend;
	parser->data = NULL;
	parser->size = 0;
}


parser_type_t
parser_get_type (parser_t *parser)
{
	if (parser == NULL)
		return PARSER_TYPE_NULL;

	return parser->backend->type;
}


parser_status_t
parser_set_data (parser_t *parser, const unsigned char *data, unsigned int size)
{
	if (parser == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	if (parser->backend->set_data == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	parser->data = data;
	parser->size = size;

	return parser->backend->set_data (parser, data, size);
}


parser_status_t
parser_get_datetime (parser_t *parser, dc_datetime_t *datetime)
{
	if (parser == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	if (parser->backend->datetime == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	return parser->backend->datetime (parser, datetime);
}

parser_status_t
parser_get_field (parser_t *parser, parser_field_type_t type, unsigned int flags, void *value)
{
	if (parser == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	if (parser->backend->field == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	return parser->backend->field (parser, type, flags, value);
}


parser_status_t
parser_samples_foreach (parser_t *parser, sample_callback_t callback, void *userdata)
{
	if (parser == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	if (parser->backend->samples_foreach == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	return parser->backend->samples_foreach (parser, callback, userdata);
}


parser_status_t
parser_destroy (parser_t *parser)
{
	if (parser == NULL)
		return PARSER_STATUS_SUCCESS;

	if (parser->backend->destroy == NULL)
		return PARSER_STATUS_UNSUPPORTED;

	return parser->backend->destroy (parser);
}


void
sample_statistics_cb (parser_sample_type_t type, parser_sample_value_t value, void *userdata)
{
	sample_statistics_t *statistics  = (sample_statistics_t *) userdata;

	switch (type) {
	case SAMPLE_TYPE_TIME:
		statistics->divetime = value.time;
		break;
	case SAMPLE_TYPE_DEPTH:
		if (statistics->maxdepth < value.depth)
			statistics->maxdepth = value.depth;
		break;
	default:
		break;
	}
}
