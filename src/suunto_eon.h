#ifndef SUUNTO_EON_H
#define SUUNTO_EON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define SUUNTO_EON_MEMORY_SIZE 0x900

device_status_t
suunto_eon_device_open (device_t **device, const char* name);

device_status_t
suunto_eon_device_write_name (device_t *device, unsigned char data[], unsigned int size);

device_status_t
suunto_eon_device_write_interval (device_t *device, unsigned char interval);

device_status_t
suunto_eon_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_EON_H */
