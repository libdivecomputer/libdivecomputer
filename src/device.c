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
#include <stdlib.h>
#include <string.h>

#include "suunto_d9.h"
#include "suunto_eon.h"
#include "suunto_eonsteel.h"
#include "suunto_solution.h"
#include "suunto_vyper2.h"
#include "suunto_vyper.h"
#include "reefnet_sensus.h"
#include "reefnet_sensuspro.h"
#include "reefnet_sensusultra.h"
#include "uwatec_aladin.h"
#include "uwatec_memomouse.h"
#include "uwatec_smart.h"
#include "oceanic_atom2.h"
#include "oceanic_veo250.h"
#include "oceanic_vtpro.h"
#include "pelagic_i330r.h"
#include "mares_darwin.h"
#include "mares_iconhd.h"
#include "mares_nemo.h"
#include "mares_puck.h"
#include "hw_frog.h"
#include "hw_ostc.h"
#include "hw_ostc3.h"
#include "cressi_edy.h"
#include "cressi_leonardo.h"
#include "cressi_goa.h"
#include "zeagle_n2ition3.h"
#include "atomics_cobalt.h"
#include "shearwater_petrel.h"
#include "shearwater_predator.h"
#include "diverite_nitekq.h"
#include "citizen_aqualand.h"
#include "divesystem_idive.h"
#include "cochran_commander.h"
#include "tecdiving_divecomputereu.h"
#include "mclean_extreme.h"
#include "liquivision_lynx.h"
#include "sporasub_sp2.h"
#include "deepsix_excursion.h"
#include "seac_screen.h"
#include "deepblu_cosmiq.h"
#include "oceans_s1.h"
#include "divesoft_freedom.h"
#include "halcyon_symbios.h"

#include "device-private.h"
#include "context-private.h"

dc_device_t *
dc_device_allocate (dc_context_t *context, const dc_device_vtable_t *vtable)
{
	dc_device_t *device = NULL;

	assert(vtable != NULL);
	assert(vtable->size >= sizeof(dc_device_t));

	// Allocate memory.
	device = (dc_device_t *) malloc (vtable->size);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return device;
	}

	device->vtable = vtable;

	device->context = context;

	device->event_mask = 0;
	device->event_callback = NULL;
	device->event_userdata = NULL;

	device->cancel_callback = NULL;
	device->cancel_userdata = NULL;

	memset (&device->devinfo, 0, sizeof (device->devinfo));
	memset (&device->clock, 0, sizeof (device->clock));

	return device;
}

void
dc_device_deallocate (dc_device_t *device)
{
	free (device);
}

dc_status_t
dc_device_open (dc_device_t **out, dc_context_t *context, dc_descriptor_t *descriptor, dc_iostream_t *iostream)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *device = NULL;

	if (out == NULL || descriptor == NULL)
		return DC_STATUS_INVALIDARGS;

	switch (dc_descriptor_get_type (descriptor)) {
	case DC_FAMILY_SUUNTO_SOLUTION:
		rc = suunto_solution_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SUUNTO_EON:
		rc = suunto_eon_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SUUNTO_VYPER:
		rc = suunto_vyper_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SUUNTO_VYPER2:
		rc = suunto_vyper2_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SUUNTO_D9:
		rc = suunto_d9_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_SUUNTO_EONSTEEL:
		rc = suunto_eonsteel_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_UWATEC_ALADIN:
		rc = uwatec_aladin_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_UWATEC_MEMOMOUSE:
		rc = uwatec_memomouse_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_UWATEC_SMART:
		rc = uwatec_smart_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_REEFNET_SENSUS:
		rc = reefnet_sensus_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_REEFNET_SENSUSPRO:
		rc = reefnet_sensuspro_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_REEFNET_SENSUSULTRA:
		rc = reefnet_sensusultra_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_OCEANIC_VTPRO:
		rc = oceanic_vtpro_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_OCEANIC_VEO250:
		rc = oceanic_veo250_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_OCEANIC_ATOM2:
		rc = oceanic_atom2_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_PELAGIC_I330R:
		rc = pelagic_i330r_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_MARES_NEMO:
		rc = mares_nemo_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_MARES_PUCK:
		rc = mares_puck_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_MARES_DARWIN:
		rc = mares_darwin_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_MARES_ICONHD:
		rc = mares_iconhd_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_HW_OSTC:
		rc = hw_ostc_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_HW_FROG:
		rc = hw_frog_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_HW_OSTC3:
		rc = hw_ostc3_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_CRESSI_EDY:
		rc = cressi_edy_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_CRESSI_LEONARDO:
		rc = cressi_leonardo_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_CRESSI_GOA:
		rc = cressi_goa_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_ZEAGLE_N2ITION3:
		rc = zeagle_n2ition3_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_ATOMICS_COBALT:
		rc = atomics_cobalt_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SHEARWATER_PREDATOR:
		rc = shearwater_predator_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SHEARWATER_PETREL:
		rc = shearwater_petrel_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_DIVERITE_NITEKQ:
		rc = diverite_nitekq_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_CITIZEN_AQUALAND:
		rc = citizen_aqualand_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_DIVESYSTEM_IDIVE:
		rc = divesystem_idive_device_open (&device, context, iostream, dc_descriptor_get_model (descriptor));
		break;
	case DC_FAMILY_COCHRAN_COMMANDER:
		rc = cochran_commander_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_TECDIVING_DIVECOMPUTEREU:
		rc = tecdiving_divecomputereu_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_MCLEAN_EXTREME:
		rc = mclean_extreme_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_LIQUIVISION_LYNX:
		rc = liquivision_lynx_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SPORASUB_SP2:
		rc = sporasub_sp2_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_DEEPSIX_EXCURSION:
		rc = deepsix_excursion_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_SEAC_SCREEN:
		rc = seac_screen_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_DEEPBLU_COSMIQ:
		rc = deepblu_cosmiq_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_OCEANS_S1:
		rc = oceans_s1_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_DIVESOFT_FREEDOM:
		rc = divesoft_freedom_device_open (&device, context, iostream);
		break;
	case DC_FAMILY_HALCYON_SYMBIOS:
		rc = halcyon_symbios_device_open (&device, context, iostream);
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	*out = device;

	return rc;
}


int
dc_device_isinstance (dc_device_t *device, const dc_device_vtable_t *vtable)
{
	if (device == NULL)
		return 0;

	return device->vtable == vtable;
}


dc_family_t
dc_device_get_type (dc_device_t *device)
{
	if (device == NULL)
		return DC_FAMILY_NULL;

	return device->vtable->type;
}


dc_status_t
dc_device_set_cancel (dc_device_t *device, dc_cancel_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	device->cancel_callback = callback;
	device->cancel_userdata = userdata;

	return DC_STATUS_SUCCESS;
}


dc_status_t
dc_device_set_events (dc_device_t *device, unsigned int events, dc_event_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	device->event_mask = events;
	device->event_callback = callback;
	device->event_userdata = userdata;

	return DC_STATUS_SUCCESS;
}


dc_status_t
dc_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->set_fingerprint == NULL)
		return DC_STATUS_UNSUPPORTED;

	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Fingerprint", data, size);

	return device->vtable->set_fingerprint (device, data, size);
}


dc_status_t
dc_device_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->read == NULL)
		return DC_STATUS_UNSUPPORTED;

	return device->vtable->read (device, address, data, size);
}


dc_status_t
dc_device_write (dc_device_t *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->write == NULL)
		return DC_STATUS_UNSUPPORTED;

	return device->vtable->write (device, address, data, size);
}


dc_status_t
dc_device_dump (dc_device_t *device, dc_buffer_t *buffer)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->dump == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (buffer == NULL)
		return DC_STATUS_INVALIDARGS;

	dc_buffer_clear (buffer);

	return device->vtable->dump (device, buffer);
}


dc_status_t
device_dump_read (dc_device_t *device, unsigned int address, unsigned char data[], unsigned int size, unsigned int blocksize)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->read == NULL)
		return DC_STATUS_UNSUPPORTED;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = size;
	device_event_emit (device, DC_EVENT_PROGRESS, &progress);

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > blocksize)
			len = blocksize;

		// Read the packet.
		dc_status_t rc = device->vtable->read (device, address + nbytes, data + nbytes, len);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (device, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
dc_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->foreach == NULL)
		return DC_STATUS_UNSUPPORTED;

	return device->vtable->foreach (device, callback, userdata);
}


dc_status_t
dc_device_timesync (dc_device_t *device, const dc_datetime_t *datetime)
{
	if (device == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (device->vtable->timesync == NULL)
		return DC_STATUS_UNSUPPORTED;

	if (datetime == NULL)
		return DC_STATUS_INVALIDARGS;

	return device->vtable->timesync (device, datetime);
}


dc_status_t
dc_device_close (dc_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (device == NULL)
		return DC_STATUS_SUCCESS;

	// Disable the cancellation callback.
	device->cancel_callback = NULL;
	device->cancel_userdata = NULL;

	if (device->vtable->close) {
		status = device->vtable->close (device);
	}

	dc_device_deallocate (device);

	return status;
}


void
device_event_emit (dc_device_t *device, dc_event_type_t event, const void *data)
{
	const dc_event_progress_t *progress = (const dc_event_progress_t *) data;

	// Check the event data for errors.
	switch (event) {
	case DC_EVENT_WAITING:
		assert (data == NULL);
		break;
	case DC_EVENT_PROGRESS:
		assert (progress != NULL);
		assert (progress->maximum != 0);
		assert (progress->maximum >= progress->current);
		break;
	case DC_EVENT_DEVINFO:
		assert (data != NULL);
		break;
	case DC_EVENT_CLOCK:
		assert (data != NULL);
		break;
	default:
		break;
	}

	if (device == NULL)
		return;

	// Cache the event data.
	switch (event) {
	case DC_EVENT_DEVINFO:
		device->devinfo = *(const dc_event_devinfo_t *) data;
		break;
	case DC_EVENT_CLOCK:
		device->clock = *(const dc_event_clock_t *) data;
		break;
	default:
		break;
	}

	// Check if there is a callback function registered.
	if (device->event_callback == NULL)
		return;

	// Check the event mask.
	if ((event & device->event_mask) == 0)
		return;

	device->event_callback (device, event, data, device->event_userdata);
}


int
device_is_cancelled (dc_device_t *device)
{
	if (device == NULL)
		return 0;

	if (device->cancel_callback == NULL)
		return 0;

	return device->cancel_callback (device->cancel_userdata);
}
