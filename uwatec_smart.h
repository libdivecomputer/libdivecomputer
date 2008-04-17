#ifndef UWATEC_SMART_H
#define UWATEC_SMART_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct smart smart;

int uwatec_smart_open (smart **device);

int uwatec_smart_close (smart *device);

int uwatec_smart_read (smart *device, unsigned char data[], unsigned int size);

int uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_SMART_H */
