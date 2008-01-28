#ifndef UWATEC_MEMOMOUSE_H
#define UWATEC_MEMOMOUSE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct memomouse memomouse;

#define UWATEC_MEMOMOUSE_MEMORY_SIZE 0x8000

int uwatec_memomouse_open (memomouse **device, const char* name);

int uwatec_memomouse_close (memomouse *device);

int uwatec_memomouse_read (memomouse *device, unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_MEMOMOUSE_H */
