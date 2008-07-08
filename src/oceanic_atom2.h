#ifndef OCEANIC_ATOM2_H
#define OCEANIC_ATOM2_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define OCEANIC_ATOM2_MEMORY_SIZE 0xFFF0
#define OCEANIC_ATOM2_PACKET_SIZE 0x10

device_status_t
oceanic_atom2_device_open (device_t **device, const char* name);

device_status_t
oceanic_atom2_device_handshake (device_t *device);

device_status_t
oceanic_atom2_device_quit (device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* OCEANIC_ATOM2_H */
