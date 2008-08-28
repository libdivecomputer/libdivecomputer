#ifndef DEVICE_H
#define DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum device_type_t {
	DEVICE_TYPE_NULL = 0,
	DEVICE_TYPE_SUUNTO_EON,
	DEVICE_TYPE_SUUNTO_VYPER,
	DEVICE_TYPE_SUUNTO_VYPER2,
	DEVICE_TYPE_SUUNTO_D9,
	DEVICE_TYPE_REEFNET_SENSUSPRO,
	DEVICE_TYPE_REEFNET_SENSUSULTRA,
	DEVICE_TYPE_UWATEC_ALADIN,
	DEVICE_TYPE_UWATEC_MEMOMOUSE,
	DEVICE_TYPE_UWATEC_SMART,
	DEVICE_TYPE_OCEANIC_ATOM2
} device_type_t;

typedef enum device_status_t {
	DEVICE_STATUS_SUCCESS = 0,
	DEVICE_STATUS_UNSUPPORTED = -1,
	DEVICE_STATUS_TYPE_MISMATCH = -2,
	DEVICE_STATUS_ERROR = -3,
	DEVICE_STATUS_IO = -4,
	DEVICE_STATUS_TIMEOUT = -5,
	DEVICE_STATUS_PROTOCOL = -6,
	DEVICE_STATUS_MEMORY = -7
} device_status_t;

typedef enum device_event_t {
	DEVICE_EVENT_WAITING,
	DEVICE_EVENT_PROGRESS
} device_event_t;

typedef struct device_t device_t;

typedef int (*dive_callback_t) (const unsigned char *data, unsigned int size, void *userdata);
typedef void (*progress_callback_t) (device_event_t event, unsigned int current, unsigned int maximum, void *userdata);

device_type_t device_get_type (device_t *device);

device_status_t device_set_progress (device_t *device, progress_callback_t callback, void *userdata);

device_status_t device_handshake (device_t *device, unsigned char data[], unsigned int size);

device_status_t device_version (device_t *device, unsigned char data[], unsigned int size);

device_status_t device_read (device_t *device, unsigned int address, unsigned char data[], unsigned int size);

device_status_t device_write (device_t *device, unsigned int address, const unsigned char data[], unsigned int size);

device_status_t device_dump (device_t *device, unsigned char data[], unsigned int size, unsigned int *result);

device_status_t device_foreach (device_t *device, dive_callback_t callback, void *userdata);

device_status_t device_close (device_t *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DEVICE_H */
