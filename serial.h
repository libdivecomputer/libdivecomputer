#ifndef SERIAL_H
#define SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct serial serial;

enum parity_t {
	SERIAL_PARITY_NONE,
	SERIAL_PARITY_EVEN,
	SERIAL_PARITY_ODD
};

enum flowcontrol_t {
	SERIAL_FLOWCONTROL_NONE,
	SERIAL_FLOWCONTROL_HARDWARE,
	SERIAL_FLOWCONTROL_SOFTWARE
};

enum queue_t {
	SERIAL_QUEUE_INPUT = 0x01,
	SERIAL_QUEUE_OUTPUT = 0x02,
	SERIAL_QUEUE_BOTH = SERIAL_QUEUE_INPUT | SERIAL_QUEUE_OUTPUT
};

int serial_errcode ();
const char* serial_errmsg ();

int serial_open (serial **device, const char* name);

int serial_close (serial *device);

int serial_configure (serial *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol);

//
// Available read modes:
//
// * Blocking (timeout < 0):
//
// The read function is blocked until all the requested bytes have
// been received. If the requested number of bytes does not arrive,
// the function will block forever.
//
// * Non-blocking (timeout == 0):
//
// The read function returns immediately with the bytes that have already
// been received, even if no bytes have been received.
//
// * Timeout (timeout > 0):
//
// The read function is blocked until all the requested bytes have 
// been received. If the requested number of bytes does not arrive
// within the specified amount of time, the function will return
// with the bytes that have already been received.
//

int serial_set_timeout (serial *device, long timeout /* milliseconds */);

int serial_read (serial *device, void* data, unsigned int size);
int serial_write (serial *device, const void* data, unsigned int size);

int serial_flush (serial *device, int queue);
int serial_drain (serial *device);

int serial_send_break (serial *device);

int serial_set_dtr (serial *device, int level);
int serial_set_rts (serial *device, int level);

int serial_get_received (serial *device);
int serial_get_transmitted (serial *device);

int serial_sleep (unsigned long timeout /* milliseconds */);

int serial_timer ();

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SERIAL_H */
