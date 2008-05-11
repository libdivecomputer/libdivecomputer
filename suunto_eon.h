#ifndef SUUNTO_EON_H
#define SUUNTO_EON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct eon eon;

#define SUUNTO_EON_MEMORY_SIZE 0x900

int suunto_eon_open (eon **device, const char* name);

int suunto_eon_close (eon *device);

int suunto_eon_read (eon *device, unsigned char data[], unsigned int size);

int suunto_eon_write_name (eon *device, unsigned char data[], unsigned int size);

int suunto_eon_write_interval (eon *device, unsigned char interval);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_EON_H */
