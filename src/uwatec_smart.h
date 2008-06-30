#ifndef UWATEC_SMART_H
#define UWATEC_SMART_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct smart smart;

#define UWATEC_SMART_VERSION_SIZE 9

int uwatec_smart_open (smart **device);

int uwatec_smart_close (smart *device);

int uwatec_smart_set_timestamp (smart *device, unsigned int timestamp);

int uwatec_smart_handshake (smart *device);

int uwatec_smart_version (smart *device, unsigned char data[], unsigned int size);

int uwatec_smart_read (smart *device, unsigned char data[], unsigned int size);

int uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_SMART_H */
