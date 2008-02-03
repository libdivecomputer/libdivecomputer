#ifndef SUUNTO_VYPER_H
#define SUUNTO_VYPER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct vyper vyper;

#define SUUNTO_VYPER_MEMORY_SIZE 0x2000
#define SUUNTO_VYPER_PACKET_SIZE 32

int suunto_vyper_open (vyper **device, const char* name);

int suunto_vyper_close (vyper *device);

int suunto_vyper_set_delay (vyper *device, unsigned int delay);

int suunto_vyper_detect_interface (vyper *device);

int suunto_vyper_read_dive (vyper *device, unsigned char data[], unsigned int size, int init);

int suunto_vyper_read_memory (vyper *device, unsigned int address, unsigned char data[], unsigned int size);

int suunto_vyper_write_memory (vyper *device, unsigned int address, const unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER_H */
