#ifndef REEFNET_SENSUSPRO_H
#define REEFNET_SENSUSPRO_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct sensuspro sensuspro;

#define REEFNET_SENSUSPRO_MEMORY_SIZE 56320
#define REEFNET_SENSUSPRO_HANDSHAKE_SIZE 10

int reefnet_sensuspro_open (sensuspro **device, const char* name);

int reefnet_sensuspro_close (sensuspro *device);

int reefnet_sensuspro_handshake (sensuspro *device, unsigned char data[], unsigned int size);

int reefnet_sensuspro_read (sensuspro *device, unsigned char data[], unsigned int size);

int reefnet_sensuspro_write_interval (sensuspro *device, unsigned char interval);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* REEFNET_SENSUSPRO_H */
