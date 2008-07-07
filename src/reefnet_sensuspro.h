#ifndef REEFNET_SENSUSPRO_H
#define REEFNET_SENSUSPRO_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define REEFNET_SENSUSPRO_MEMORY_SIZE 56320
#define REEFNET_SENSUSPRO_HANDSHAKE_SIZE 10

device_status_t
reefnet_sensuspro_device_open (device_t **device, const char* name);

device_status_t
reefnet_sensuspro_write_interval (device_t *device, unsigned char interval);

device_status_t
reefnet_sensuspro_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* REEFNET_SENSUSPRO_H */
