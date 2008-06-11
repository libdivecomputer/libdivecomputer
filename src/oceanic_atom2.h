#ifndef OCEANIC_ATOM2_H
#define OCEANIC_ATOM2_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct atom2 atom2;

#define OCEANIC_ATOM2_MEMORY_SIZE 0xFFF0
#define OCEANIC_ATOM2_PACKET_SIZE 0x10

int oceanic_atom2_open (atom2 **device, const char* name);

int oceanic_atom2_close (atom2 *device);

int oceanic_atom2_read_version (atom2 *device, unsigned char data[], unsigned int size);

int oceanic_atom2_read_memory (atom2 *device, unsigned int address, unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* OCEANIC_ATOM2_H */
