#ifndef SUUNTO_VYPER2_H
#define SUUNTO_VYPER2_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define SUUNTO_VYPER2_MEMORY_SIZE 0x8000
#define SUUNTO_VYPER2_PACKET_SIZE 0x78
#define SUUNTO_VYPER2_VERSION_SIZE 0x04

device_status_t
suunto_vyper2_device_open (device_t **device, const char* name);

device_status_t
suunto_vyper2_device_reset_maxdepth (device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER2_H */
