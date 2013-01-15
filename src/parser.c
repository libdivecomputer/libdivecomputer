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

#include <libdivecomputer/suunto.h>
#include <libdivecomputer/reefnet.h>
#include <libdivecomputer/uwatec.h>
#include <libdivecomputer/oceanic.h>
#include <libdivecomputer/mares.h>
#include <libdivecomputer/hw.h>
#include <libdivecomputer/cressi.h>
#include <libdivecomputer/zeagle.h>
#include <libdivecomputer/atomics.h>
#include <libdivecomputer/shearwater.h>

#include "parser-private.h"
#include "device-private.h"

dc_status_t
dc_parser_new (dc_parser_t **out, dc_device_t *device)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_parser_t *parser = NULL;

	if (out == NULL || device == NULL)
		return DC_STATUS_INVALIDARGS;

	dc_context_t *context = device->context;

	switch (dc_device_get_type (device)) {
	case DC_FAMILY_SUUNTO_SOLUTION:
		rc = suunto_solution_parser_create (&parser, context);
		break;
	case DC_FAMILY_SUUNTO_EON:
		rc = suunto_eon_parser_create (&parser, context, 0);
		break;
	case DC_FAMILY_SUUNTO_VYPER:
		if (device->devinfo.model == 0x01)
			rc = suunto_eon_parser_create (&parser, context, 1);
		else
			rc = suunto_vyper_parser_create (&parser, context);
		break;
	case DC_FAMILY_SUUNTO_VYPER2:
	case DC_FAMILY_SUUNTO_D9:
		rc = suunto_d9_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_UWATEC_ALADIN:
	case DC_FAMILY_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_parser_create (&parser, context, device->clock.devtime, device->clock.systime);
		break;
	case DC_FAMILY_UWATEC_SMART:
		rc = uwatec_smart_parser_create (&parser, context, device->devinfo.model, device->clock.devtime, device->clock.systime);
		break;
	case DC_FAMILY_REEFNET_SENSUS:
		rc = reefnet_sensus_parser_create (&parser, context, device->clock.devtime, device->clock.systime);
		break;
	case DC_FAMILY_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_parser_create (&parser, context, device->clock.devtime, device->clock.systime);
		break;
	case DC_FAMILY_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_parser_create (&parser, context, device->clock.devtime, device->clock.systime);
		break;
	case DC_FAMILY_OCEANIC_VTPRO:
		rc = oceanic_vtpro_parser_create (&parser, context);
		break;
	case DC_FAMILY_OCEANIC_VEO250:
		rc = oceanic_veo250_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_OCEANIC_ATOM2:
		rc = oceanic_atom2_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_MARES_NEMO:
	case DC_FAMILY_MARES_PUCK:
		rc = mares_nemo_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_MARES_DARWIN:
		rc = mares_darwin_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_MARES_ICONHD:
		rc = mares_iconhd_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_HW_OSTC:
		rc = hw_ostc_parser_create (&parser, context, 0);
		break;
	case DC_FAMILY_HW_FROG:
		rc = hw_ostc_parser_create (&parser, context, 1);
		break;
	case DC_FAMILY_CRESSI_EDY:
	case DC_FAMILY_ZEAGLE_N2ITION3:
		rc = cressi_edy_parser_create (&parser, context, device->devinfo.model);
		break;
	case DC_FAMILY_CRESSI_LEONARDO:
		rc = cressi_leonardo_parser_create (&parser, context);
		break;
	case DC_FAMILY_ATOMICS_COBALT:
		rc = atomics_cobalt_parser_create (&parser, context);
		break;
	case DC_FAMILY_SHEARWATER_PREDATOR:
		rc = shearwater_predator_parser_create (&parser, context);
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	*out = parser;

	return rc;
}


void
parser_init (dc_parser_t *parser, dc_context_t *context, const parser_backend_t *backend)
{
	parser->backend = backend;
	parser->context = context;
	parser->data = NULL;
	parser->size = 0;
}


dc_family_t
dc_parser_get_type (dc_parser_t *parser)
{
	if (parser == NULL)
		return DC_FAMILY_NULL;

	return parser->backend->type;
}


dc_status_t
dc_parser_set_data (dc_parser_t *parser, const unsigned char *data, unsigned int size)
{
	if (parser == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (parser->backend->set_data == NULL)
		return DC_STATUS_UNSUPPORTED;

	parser->data = data;
	parser->size = size;

	return parser->backend->set_data (parser, data, size);
}


dc_status_t
dc_parser_get_datetime (dc_parser_t *parser, dc_datetime_t *datetime)
{
	if (parser == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (parser->backend->datetime == NULL)
		return DC_STATUS_UNSUPPORTED;

	return parser->backend->datetime (parser, datetime);
}

dc_status_t
dc_parser_get_field (dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value)
{
	if (parser == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (parser->backend->field == NULL)
		return DC_STATUS_UNSUPPORTED;

	return parser->backend->field (parser, type, flags, value);
}


dc_status_t
dc_parser_samples_foreach (dc_parser_t *parser, dc_sample_callback_t callback, void *userdata)
{
	if (parser == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (parser->backend->samples_foreach == NULL)
		return DC_STATUS_UNSUPPORTED;

	return parser->backend->samples_foreach (parser, callback, userdata);
}


dc_status_t
dc_parser_destroy (dc_parser_t *parser)
{
	if (parser == NULL)
		return DC_STATUS_SUCCESS;

	if (parser->backend->destroy == NULL)
		return DC_STATUS_UNSUPPORTED;

	return parser->backend->destroy (parser);
}


void
sample_statistics_cb (dc_sample_type_t type, dc_sample_value_t value, void *userdata)
{
	sample_statistics_t *statistics  = (sample_statistics_t *) userdata;

	switch (type) {
	case DC_SAMPLE_TIME:
		statistics->divetime = value.time;
		break;
	case DC_SAMPLE_DEPTH:
		if (statistics->maxdepth < value.depth)
			statistics->maxdepth = value.depth;
		break;
	default:
		break;
	}
}
