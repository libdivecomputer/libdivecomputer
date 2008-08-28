#include <assert.h>

#include "device-private.h"

#define NULL 0


void
device_init (device_t *device, const device_backend_t *backend)
{
	device->backend = backend;
	device->progress = NULL;
	device->userdata = NULL;
}


device_type_t
device_get_type (device_t *device)
{
	if (device == NULL)
		return DEVICE_TYPE_NULL;

	return device->backend->type;
}


device_status_t
device_set_progress (device_t *device, progress_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	device->progress = callback;
	device->userdata = userdata;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
device_handshake (device_t *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->handshake == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->handshake (device, data, size);
}


device_status_t
device_version (device_t *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->version == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->version (device, data, size);
}


device_status_t
device_read (device_t *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->read == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->read (device, address, data, size);
}


device_status_t
device_write (device_t *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->write == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->write (device, address, data, size);
}


device_status_t
device_dump (device_t *device, unsigned char data[], unsigned int size, unsigned int *result)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->dump == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->dump (device, data, size, result);
}


device_status_t
device_foreach (device_t *device, dive_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->foreach == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->foreach (device, callback, userdata);
}


device_status_t
device_close (device_t *device)
{
	if (device == NULL)
		return DEVICE_STATUS_SUCCESS;

	if (device->backend->close == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->close (device);
}


void
progress_init (device_progress_state_t *progress, device_t *device, unsigned int maximum)
{
	if (progress == NULL)
		return;

	progress->callback = (device ? device->progress : NULL);
	progress->userdata = (device ? device->userdata : NULL);
	progress->maximum = maximum;
	progress->current = 0;
}


void
progress_event (device_progress_state_t *progress, device_event_t event, unsigned int value)
{
	if (progress == NULL)
		return;

	switch (event) {
	case DEVICE_EVENT_WAITING:
		break;
	case DEVICE_EVENT_PROGRESS:
		progress->current += value;
		break;
	default:
		return;
	}

	assert (progress->maximum != 0);
	assert (progress->maximum >= progress->current);

	if (progress->callback)
		progress->callback (event, progress->current, progress->maximum, progress->userdata);
}


void
progress_set_maximum (device_progress_state_t *progress, unsigned int value)
{
	if (progress == NULL)
		return;

	assert (value <= progress->maximum);
	assert (value >= progress->current);

	progress->maximum = value;
}
