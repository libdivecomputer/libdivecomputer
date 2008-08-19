#ifndef DEVICE_PRIVATE_H
#define DEVICE_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "device.h"

struct device_t;
struct device_backend_t;

typedef struct device_backend_t device_backend_t;

struct device_t {
	const device_backend_t *backend;
	// Progress callback data.
	progress_callback_t progress;
	void *userdata;
};

struct device_backend_t {
    device_type_t type;

	device_status_t (*handshake) (device_t *device, unsigned char data[], unsigned int size);

	device_status_t (*version) (device_t *device, unsigned char data[], unsigned int size);

	device_status_t (*read) (device_t *device, unsigned int address, unsigned char data[], unsigned int size);

	device_status_t (*write) (device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

	device_status_t (*dump) (device_t *device, unsigned char data[], unsigned int size);

	device_status_t (*foreach) (device_t *device, dive_callback_t callback, void *userdata);

	device_status_t (*close) (device_t *device);
};

#define INFINITE ((unsigned int)-1)

typedef struct device_progress_state_t {
	progress_callback_t callback;
	void *userdata;
	unsigned int current, maximum;
} device_progress_state_t;

void
progress_init (device_progress_state_t *progress, device_t *device, unsigned int maximum);

void
progress_event (device_progress_state_t *progress, device_event_t event, unsigned int value);

void
progress_set_maximum (device_progress_state_t *progress, unsigned int value);

void
device_init (device_t *device, const device_backend_t *backend);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DEVICE_PRIVATE_H */
