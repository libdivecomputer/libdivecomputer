#ifndef UWATEC_SMART_H
#define UWATEC_SMART_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

#define UWATEC_SMART_VERSION_SIZE 9

device_status_t
uwatec_smart_device_open (device_t **device);

device_status_t
uwatec_smart_device_set_timestamp (device_t *device, unsigned int timestamp);

device_status_t
uwatec_smart_device_handshake (device_t *device);

device_status_t
uwatec_smart_device_version (device_t *device, unsigned char data[], unsigned int size);

device_status_t
uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_SMART_H */
