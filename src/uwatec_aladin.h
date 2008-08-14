#ifndef UWATEC_ALADIN_H
#define UWATEC_ALADIN_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define UWATEC_ALADIN_MEMORY_SIZE 2048

device_status_t
uwatec_aladin_device_open (device_t **device, const char* name);

device_status_t
uwatec_aladin_device_set_timestamp (device_t *device, unsigned int timestamp);

device_status_t
uwatec_aladin_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata, unsigned int timestamp);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_ALADIN_H */
