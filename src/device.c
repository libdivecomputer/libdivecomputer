#include "device-private.h"

#define NULL 0

device_type_t
device_get_type (device_t *device)
{
	if (device == NULL)
		return DEVICE_TYPE_NULL;

	return device->backend->type;
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
device_dump (device_t *device, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	if (device->backend->dump == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	return device->backend->dump (device, data, size);
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
