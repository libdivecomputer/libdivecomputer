#ifndef SUUNTO_VYPER2_H
#define SUUNTO_VYPER2_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct vyper2 vyper2;

#define SUUNTO_VYPER2_MEMORY_SIZE 0x8000
#define SUUNTO_VYPER2_PACKET_SIZE 0x78

int suunto_vyper2_open (vyper2 **device, const char* name);

int suunto_vyper2_close (vyper2 *device);

int suunto_vyper2_read_version (vyper2 *device, unsigned char data[], unsigned int size);

int suunto_vyper2_reset_maxdepth (vyper2 *device);

int suunto_vyper2_read_memory (vyper2 *device, unsigned int address, unsigned char data[], unsigned int size);

int suunto_vyper2_write_memory (vyper2 *device, unsigned int address, const unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_VYPER2_H */
