#ifndef UWATEC_MEMOMOUSE_H
#define UWATEC_MEMOMOUSE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct memomouse memomouse;

int uwatec_memomouse_open (memomouse **device, const char* name);

int uwatec_memomouse_close (memomouse *device);

int uwatec_memomouse_set_timestamp (memomouse *device, unsigned int timestamp);

int uwatec_memomouse_read (memomouse *device, unsigned char data[], unsigned int size);

int uwatec_memomouse_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_MEMOMOUSE_H */
