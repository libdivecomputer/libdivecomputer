/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#ifndef DC_IOSTREAM_H
#define DC_IOSTREAM_H

#include <stddef.h>
#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing a I/O stream.
 */
typedef struct dc_iostream_t dc_iostream_t;

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
 * The line signals.
 */
typedef enum dc_line_t {
	DC_LINE_DCD = 0x01, /**< Data carrier detect */
	DC_LINE_CTS = 0x02, /**< Clear to send */
	DC_LINE_DSR = 0x04, /**< Data set ready */
	DC_LINE_RNG = 0x08, /**< Ring indicator */
} dc_line_t;

/**
 * Get the transport type.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @returns The transport type of the I/O stream.
 */
dc_transport_t
dc_iostream_get_transport (dc_iostream_t *iostream);

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
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  timeout   The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_set_timeout (dc_iostream_t *iostream, int timeout);

/**
 * Set the state of the break condition.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  value     The break condition state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_set_break (dc_iostream_t *iostream, unsigned int value);

/**
 * Set the state of the DTR line.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  value     The DTR line state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_set_dtr (dc_iostream_t *iostream, unsigned int value);

/**
 * Set the state of the RTS line.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  value     The RTS line state.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_set_rts (dc_iostream_t *iostream, unsigned int value);

/**
 * Query the state of the line signals.
 *
 * @param[in]   iostream  A valid I/O stream.
 * @param[out]  value     A location to store the bitmap with the state
 *                        of the line signals.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_get_lines (dc_iostream_t *iostream, unsigned int *value);

/**
 * Query the number of available bytes in the input buffer.
 *
 * @param[in]   iostream  A valid I/O stream.
 * @param[out]  value     A location to store the number of bytes in the
 *                        input buffer.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_get_available (dc_iostream_t *iostream, size_t *value);

/**
 * Configure the line settings.
 *
 * @param[in]  iostream     A valid I/O stream.
 * @param[in]  baudrate     The baud rate setting.
 * @param[in]  databits     The number of data bits.
 * @param[in]  parity       The parity setting.
 * @param[in]  stopbits     The number of stop bits.
 * @param[in]  flowcontrol  The flow control setting.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_configure (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);

/**
 * Poll the I/O stream for available data.
 *
 * There are three distinct modes available:
 *
 *  1. Blocking (timeout < 0):
 *
 *     The poll operation is blocked until one or more bytes have been
 *     received. If no bytes are received, the operation will block
 *     forever.
 *
 *  2. Non-blocking (timeout == 0):
 *
 *     The poll operation returns immediately, even if no bytes have
 *     been received.
 *
 *  3. Timeout (timeout > 0):
 *
 *     The poll operation is blocked until one or more bytes have been
 *     received. If no bytes are received within the specified amount of
 *     time, the operation will return with a timeout.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  timeout   The timeout in milliseconds.
 * @returns #DC_STATUS_SUCCESS on success, #DC_STATUS_TIMEOUT on
 * timeout, or another #dc_status_t code on failure.
 */
dc_status_t
dc_iostream_poll (dc_iostream_t *iostream, int timeout);

/**
 * Read data from the I/O stream.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[out] data      The memory buffer to read the data into.
 * @param[in]  size      The number of bytes to read.
 * @param[out] actual    An (optional) location to store the actual
 *                       number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);

/**
 * Write data to the I/O stream.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  data      The memory buffer to write the data from.
 * @param[in]  size      The number of bytes to write.
 * @param[out] actual    An (optional) location to store the actual
 *                       number of bytes transferred.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);

/**
 * Perform an I/O stream specific request.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @param[in]  request   The request to perform.
 * @param[in,out]  data  The request specific data.
 * @param[in]  size      The size of the request specific data.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);

/**
 * Flush the internal output buffer and wait until the data has been
 * transmitted.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_flush (dc_iostream_t *iostream);

/**
 * Discards all data from the internal buffers.
 *
 * @param[in]  iostream   A valid I/O stream.
 * @param[in]  direction  The direction of the buffer(s).
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_purge (dc_iostream_t *iostream, dc_direction_t direction);

/**
 * Suspend execution of the current thread for the specified amount of
 * time.
 *
 * @param[in]  iostream      A valid I/O stream.
 * @param[in]  milliseconds  The number of milliseconds to sleep.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_sleep (dc_iostream_t *iostream, unsigned int milliseconds);

/**
 * Close the I/O stream and free all resources.
 *
 * @param[in]  iostream  A valid I/O stream.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_iostream_close (dc_iostream_t *iostream);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IOSTREAM_H */
