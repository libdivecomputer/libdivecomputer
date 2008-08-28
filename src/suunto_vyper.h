#ifndef SUUNTO_VYPER_H
#define SUUNTO_VYPER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define SUUNTO_VYPER_MEMORY_SIZE 0x2000
#define SUUNTO_VYPER_PACKET_SIZE 32

device_status_t
suunto_vyper_device_open (device_t **device, const char* name);

device_status_t
suunto_vyper_device_set_delay (device_t *device, unsigned int delay);

device_status_t
suunto_vyper_device_detect_interface (device_t *device);

device_status_t
suunto_vyper_device_read_dive (device_t *device, unsigned char data[], unsigned int size, unsigned int *result, int init);

device_status_t
suunto_vyper_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

device_status_t
suunto_spyder_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER_H */
