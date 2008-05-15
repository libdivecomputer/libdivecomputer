#ifndef REEFNET_SENSUSULTRA_H
#define REEFNET_SENSUSULTRA_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct sensusultra sensusultra;

#define REEFNET_SENSUSULTRA_PACKET_SIZE 512
#define REEFNET_SENSUSULTRA_MEMORY_USER_SIZE 16384 /* 32 PAGES */
#define REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE 2080768 /* 4064 PAGES */
#define REEFNET_SENSUSULTRA_MEMORY_SIZE 2097152 /* USER + DATA */
#define REEFNET_SENSUSULTRA_HANDSHAKE_SIZE 24
#define REEFNET_SENSUSULTRA_SENSE_SIZE 6

int reefnet_sensusultra_open (sensusultra **device, const char* name);

int reefnet_sensusultra_close (sensusultra *device);

int reefnet_sensusultra_handshake (sensusultra *device, unsigned char *data, unsigned int size);

int reefnet_sensusultra_read_data (sensusultra *device, unsigned char *data, unsigned int size);
int reefnet_sensusultra_read_user (sensusultra *device, unsigned char *data, unsigned int size);

int reefnet_sensusultra_write_user (sensusultra *device, const unsigned char *data, unsigned int size);

int reefnet_sensusultra_write_interval (sensusultra *device, unsigned int value);
int reefnet_sensusultra_write_threshold (sensusultra *device, unsigned int value);
int reefnet_sensusultra_write_endcount (sensusultra *device, unsigned int value);
int reefnet_sensusultra_write_averaging (sensusultra *device, unsigned int value);

int reefnet_sensusultra_sense (sensusultra *device, unsigned char *data, unsigned int size);

int reefnet_sensusultra_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* REEFNET_SENSUSULTRA_H */
