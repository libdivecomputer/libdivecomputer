/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#ifndef DC_SERIAL_H
#define DC_SERIAL_H

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a serial connection.
 */
typedef struct dc_serial_t dc_serial_t;

/**
 * The parity checking scheme.
 */
typedef enum dc_parity_t {
	DC_PARITY_NONE, /**< No parity */
	DC_PARITY_ODD,  /**< Odd parity */
	DC_PARITY_EVEN, /**< Even parity */
	DC_PARITY_MARK, /**< Mark parity (always 1) */
	DC_PARITY_SPACE /**< Space parity (alwasy 0) */
} dc_parity_t;

/**
 * The number of stop bits.
 */
typedef enum dc_stopbits_t {
	DC_STOPBITS_ONE,          /**< 1 stop bit */
	DC_STOPBITS_ONEPOINTFIVE, /**< 1.5 stop bits*/
	DC_STOPBITS_TWO           /**< 2 stop bits */
} dc_stopbits_t;

/**
 * The flow control.
 */
typedef enum dc_flowcontrol_t {
	DC_FLOWCONTROL_NONE,     /**< No flow control */
	DC_FLOWCONTROL_HARDWARE, /**< Hardware (RTS/CTS) flow control */
	DC_FLOWCONTROL_SOFTWARE  /**< Software (XON/XOFF) flow control */
} dc_flowcontrol_t;

/**
 * The direction of the data transmission.
 */
typedef enum dc_direction_t {
	DC_DIRECTION_INPUT = 0x01,  /**< Input direction */
	DC_DIRECTION_OUTPUT = 0x02, /**< Output direction */
	DC_DIRECTION_ALL = DC_DIRECTION_INPUT | DC_DIRECTION_OUTPUT /**< All directions */
} dc_direction_t;

/**
 * The serial line signals.
 */
typedef enum dc_line_t {
	DC_LINE_DCD = 0x01, /**< Data carrier detect */
	DC_LINE_CTS = 0x02, /**< Clear to send */
	DC_LINE_DSR = 0x04, /**< Data set ready */
	DC_LINE_RNG = 0x08, /**< Ring indicator */
} dc_line_t;

/**
 * Serial enumeration callback.
 *
 * @param[in]  name      The name of the device node.
 * @param[in]  userdata  The user data pointer.
 */
typedef void (*dc_serial_callback_t) (const char *name, void *userdata);

/**
 * Enumerate the serial ports.
 *
 * @param[in]  callback  The callback function to call.
 * @param[in]  userdata  User data to pass to the callback function.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_enumerate (dc_serial_callback_t callback, void *userdata);

/**
 * Open a serial connection.
 *
 * @param[out]  serial   A location to store the serial connection.
 * @param[in]   context  A valid context object.
 * @param[in]   name     The name of the device node.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_open (dc_serial_t **serial, dc_context_t *context, const char *name);

/**
 * Close the serial connection and free all resources.
 *
 * @param[in]  serial  A valid serial connection.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_close (dc_serial_t *serial);

/**
 * Configure the serial line settings of the connection.
 *
 * @param[in]  serial       A valid serial connection.
 * @param[in]  baudrate     The baud rate setting.
 * @param[in]  databits     The number of data bits.
 * @param[in]  parity       The parity setting.
 * @param[in]  stopbits     The number of stop bits.
 * @param[in]  flowcontrol  The flow control setting.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_configure (dc_serial_t *serial, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);

/**
 * Set the read timeout.
 *
 * There are three distinct modes available:
 *
 *  1. Blocking (timeout < 0):
 *
 *     The read operation is blocked until all the requested bytes have
 *     been received. If the requested number of bytes does not arrive,
 *     the operation will block forever.
 *
 *  2. Non-blocking (timeout == 0):
 *
 *     The read operation returns immediately with the bytes that have
 *     already been received, even if no bytes have been received.
 *
 *  3. Timeout (timeout > 0):
 *
 *     The read operation is blocked until all the requested bytes have
 *     been received. If the requested number of bytes does not arrive
 *     within the specified amount of time, the operation will return
 *     with the bytes that have already been received.
 *
 * @param[in]  serial   A valid serial connection.
 * @param[in]  timeout  The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_timeout (dc_serial_t *serial, int timeout);

/**
 * Set the state of the half duplex emulation.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  value   The half duplex state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_halfduplex (dc_serial_t *serial, unsigned int value);

/**
 * Set the receive latency.
 *
 * The effect of this setting is highly platform and driver specific. On
 * Windows it does nothing at all, on Linux it controls the low latency
 * flag (e.g. only zero vs non-zero latency), and on Mac OS X it sets
 * the receive latency as requested.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  value   The latency in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_latency (dc_serial_t *serial, unsigned int value);

/**
 * Read data from the serial connection.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[out] data    The memory buffer to read the data into.
 * @param[in]  size    The number of bytes to read.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_read (dc_serial_t *serial, void *data, size_t size, size_t *actual);

/**
 * Write data to the serial connection.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  data    The memory buffer to write the data from.
 * @param[in]  size    The number of bytes to write.
 * @param[out] actual  An (optional) location to store the actual
 *                     number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_write (dc_serial_t *serial, const void *data, size_t size, size_t *actual);

/**
 * Flush the internal output buffer and wait until the data has been
 * transmitted.
 *
 * @param[in]  serial  A valid serial connection.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_flush (dc_serial_t *serial);

/**
 * Discards all data from the internal buffers.
 *
 * @param[in]  serial     A valid serial connection.
 * @param[in]  direction  The direction of the buffer(s).
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_purge (dc_serial_t *serial, dc_direction_t direction);

/**
 * Set the state of the break condition.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  value   The break condition state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_break (dc_serial_t *serial, unsigned int value);

/**
 * Set the state of the DTR line.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  value   The DTR line state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_dtr (dc_serial_t *serial, unsigned int value);

/**
 * Set the state of the RTS line.
 *
 * @param[in]  serial  A valid serial connection.
 * @param[in]  value   The RTS line state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_set_rts (dc_serial_t *serial, unsigned int value);

/**
 * Query the number of available bytes in the input buffer.
 *
 * @param[in]   serial  A valid serial connection.
 * @param[out]  value   A location to store the number of bytes in the
 *                      input buffer.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_get_available (dc_serial_t *serial, size_t *value);

/**
 * Query the state of the line signals.
 *
 * @param[in]   serial  A valid serial connection.
 * @param[out]  value   A location to store the bitmap with the state of
 *                      the line signals.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_get_lines (dc_serial_t *serial, unsigned int *value);

/**
 * Suspend execution of the current thread for the specified amount of
 * time.
 *
 * @param[in]  serial        A valid serial connection.
 * @param[in]  milliseconds  The number of milliseconds to sleep.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_serial_sleep (dc_serial_t *serial, unsigned int milliseconds);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_SERIAL_H */
