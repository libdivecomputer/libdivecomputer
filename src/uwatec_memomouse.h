#ifndef UWATEC_MEMOMOUSE_H
#define UWATEC_MEMOMOUSE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

device_status_t
uwatec_memomouse_device_open (device_t **device, const char* name);

device_status_t
uwatec_memomouse_device_set_timestamp (device_t *device, unsigned int timestamp);

device_status_t
uwatec_memomouse_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_MEMOMOUSE_H */
