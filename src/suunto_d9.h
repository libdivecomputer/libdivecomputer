#ifndef SUUNTO_D9_H
#define SUUNTO_D9_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct d9 d9;

#define SUUNTO_D9_MEMORY_SIZE 0x8000
#define SUUNTO_D9_PACKET_SIZE 0x78
#define SUUNTO_D9_VERSION_SIZE 0x04

int suunto_d9_open (d9 **device, const char* name);

int suunto_d9_close (d9 *device);

int suunto_d9_read_version (d9 *device, unsigned char data[], unsigned int size);

int suunto_d9_reset_maxdepth (d9 *device);

int suunto_d9_read_memory (d9 *device, unsigned int address, unsigned char data[], unsigned int size);

int suunto_d9_write_memory (d9 *device, unsigned int address, const unsigned char data[], unsigned int size);

int suunto_d9_read_dives (d9 *device, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_D9_H */
