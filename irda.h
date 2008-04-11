#ifndef IRDA_H
#define IRDA_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct irda irda;

typedef void (*irda_callback_t) (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata);

int irda_errcode ();

const char* irda_errmsg ();

int irda_init ();

int irda_cleanup ();

int irda_socket_open (irda **device);

int irda_socket_close (irda *device);

int irda_socket_set_timeout (irda *device, long timeout);

int irda_socket_discover (irda *device, irda_callback_t callback, void *userdata);

int irda_socket_connect_name (irda *device, unsigned int address, const char *name);
int irda_socket_connect_lsap (irda *device, unsigned int address, unsigned int lsap);

int irda_socket_available (irda* device);

int irda_socket_read (irda* device, void* data, unsigned int size);

int irda_socket_write (irda* device, const void *data, unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* IRDA_H */
