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

#include <stdlib.h> // malloc, free
#include <string.h>	// strerror
#include <errno.h>	// errno
#include <unistd.h>	// open, close, read, write
#include <fcntl.h>	// fcntl
#include <termios.h>	// tcgetattr, tcsetattr, cfsetispeed, cfsetospeed, tcflush, tcsendbreak
#include <sys/ioctl.h>	// ioctl
#include <sys/time.h>	// gettimeofday
#include <time.h>	// nanosleep

#ifndef TIOCINQ
#define TIOCINQ FIONREAD
#endif

#include "serial.h"
#include "utils.h"

#define TRACE(expr) \
{ \
	int error = errno; \
	message ("TRACE (%s:%d, %s): %s (%d)\n", __FILE__, __LINE__, \
		expr, serial_errmsg (), serial_errcode ()); \
	errno = error; \
}

struct serial {
	/*
	 * The file descriptor corresponding to the serial port.
	 */
	int fd;
	long timeout;
	/*
	 * Serial port settings are saved into this variable immediately
	 * after the port is opened. These settings are restored when the
	 * serial port is closed.
	 */
	struct termios tty;
};

//
// Error reporting.
//

int serial_errcode (void)
{
	return errno;
}


const char* serial_errmsg (void)
{
	return strerror (errno);
}

//
// Open the serial port.
//

int
serial_open (serial** out, const char* name)
{
	if (out == NULL)
		return -1; // EINVAL (Invalid argument)

	// Allocate memory.
	struct serial *device = (struct serial *) malloc (sizeof (struct serial));
	if (device == NULL) {
		TRACE ("malloc");
		return -1; // ENOMEM (Not enough space)
	}

	// Default to blocking reads.
	device->timeout = -1;

	// Open the device in non-blocking mode, to return immediately
	// without waiting for the modem connection to complete.
	device->fd = open (name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (device->fd == -1) {
		TRACE ("open");
		free (device);
		return -1; // Error during open call.
	}

	// Retrieve the current terminal attributes, to 
	// be able to restore them when closing the device.
	// It is also used to check if the obtained
	// file descriptor represents a terminal device.
	if (tcgetattr (device->fd, &device->tty) != 0) {
		TRACE ("tcgetattr");
		close (device->fd);
		free (device);
		return -1;
	}

	*out = device;

	return 0;
}

//
// Close the serial port.
//

int
serial_close (serial* device)
{
	if (device == NULL)
		return 0;

	// Restore the initial terminal attributes.
	if (tcsetattr (device->fd, TCSANOW, &device->tty) != 0) {
		TRACE ("tcsetattr");
		close (device->fd);
		free (device);
		return -1;
	}

	// Close the device.
	if (close (device->fd) != 0) {
		TRACE ("close");
		free (device);
		return -1;
	}

	// Free memory.	
	free (device);

	return 0;
}

//
// Configure the serial port (baudrate, databits, parity, stopbits and flowcontrol).
//

int
serial_configure (serial *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	// Retrieve the current settings.
	struct termios tty;
	if (tcgetattr (device->fd, &tty) != 0) {
		TRACE ("tcgetattr");
		return -1;
	}

	// Setup raw input/output mode without echo.
	tty.c_iflag &= ~(IGNBRK | BRKINT | ISTRIP | INLCR | IGNCR | ICRNL);
	tty.c_oflag &= ~(OPOST);
	tty.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);

	// Enable the receiver (CREAD) and ignore modem control lines (CLOCAL).
	tty.c_cflag |= (CLOCAL | CREAD); 

    // VMIN is the minimum number of characters for non-canonical read 
    // and VTIME is the timeout in deciseconds for non-canonical read.
    // Setting both of these parameters to zero implies that a read
    // will return immediately, only giving the currently available
    // characters (non-blocking read behaviour). However, a non-blocking
    // read (or write) can also be achieved by using O_NONBLOCK.
    // But together with VMIN = 1, it becomes possible to recognize
    // the difference between a timeout and modem disconnect (EOF)
    // when read() returns zero.
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 0;

	// Set the baud rate.
	speed_t baud = 0;
	switch (baudrate) {
	case 0: baud = B0; break;
	case 50: baud = B50; break;
	case 75: baud = B75; break;
	case 110: baud = B110; break;
	case 134: baud = B134; break;
	case 150: baud = B150; break;
	case 200: baud = B200; break;
	case 300: baud = B300; break;
	case 600: baud = B600; break;
	case 1200: baud = B1200; break;
	case 1800: baud = B1800; break;
	case 2400: baud = B2400; break;
	case 4800: baud = B4800; break;
	case 9600: baud = B9600; break;
	case 19200: baud = B19200; break;
	case 38400: baud = B38400; break;
#ifdef B57600
	case 57600: baud = B57600; break;
#endif
#ifdef B115200
	case 115200: baud = B115200; break;
#endif
#ifdef B230400
	case 230400: baud = B230400; break;
#endif
	default:
		return -1;
	}
	if (cfsetispeed (&tty, baud) != 0 ||
		cfsetospeed (&tty, baud) != 0) {
		TRACE ("cfsetispeed/cfsetospeed");
		return -1;
	}

	// Set the character size.
	tty.c_cflag &= ~CSIZE;
	switch (databits) {
	case 5:
		tty.c_cflag |= CS5;
		break;
	case 6:
		tty.c_cflag |= CS6;
		break;
	case 7:
		tty.c_cflag |= CS7;
		break;
	case 8:
		tty.c_cflag |= CS8;
		break;
	default:
		return -1;
	}

	// Set the parity type.
	tty.c_cflag &= ~(PARENB | PARODD);
	tty.c_iflag &= ~(IGNPAR | PARMRK | INPCK);
	switch (parity) {
	case SERIAL_PARITY_NONE: // No parity
		tty.c_iflag |= IGNPAR;
		break;
	case SERIAL_PARITY_EVEN: // Even parity
		tty.c_cflag |= PARENB;
		tty.c_iflag |= INPCK;
		break;
	case SERIAL_PARITY_ODD: // Odd parity
		tty.c_cflag |= (PARENB | PARODD);
		tty.c_iflag |= INPCK;
		break;
	default:
		return -1;
	}

	// Set the number of stop bits.
	switch (stopbits) {
	case 1: // One stopbit
		tty.c_cflag &= ~CSTOPB;
		break;
	case 2: // Two stopbits
		tty.c_cflag |= CSTOPB;
		break;
	default:
		return -1;
	}

	// Set the flow control.
	switch (flowcontrol) {
	case SERIAL_FLOWCONTROL_NONE: // No flow control.
		#ifdef CRTSCTS
			tty.c_cflag &= ~CRTSCTS;
		#endif
		tty.c_iflag &= ~(IXON | IXOFF | IXANY);
		break;
	case SERIAL_FLOWCONTROL_HARDWARE: // Hardware (RTS/CTS) flow control.
		#ifdef CRTSCTS
			tty.c_cflag |= CRTSCTS;
			tty.c_iflag &= ~(IXON | IXOFF | IXANY);
			break;
		#else
			return -1; // Hardware flow control is unsupported.
		#endif
	case SERIAL_FLOWCONTROL_SOFTWARE: // Software (XON/XOFF) flow control.
		#ifdef CRTSCTS
			tty.c_cflag &= ~CRTSCTS;
		#endif
		tty.c_iflag |= (IXON | IXOFF);
		break;
	default:
		return -1;
	}

	// Apply the new settings.
	if (tcsetattr (device->fd, TCSANOW, &tty) != 0) {
		TRACE ("tcsetattr");
		return -1;
	}

	// tcsetattr() returns success if any of the requested changes could be
	// successfully carried out. Therefore, when making multiple changes
	// it may be necessary to follow this call with a further call to
	// tcgetattr() to check that all changes have been performed successfully.

	struct termios active;
	if (tcgetattr (device->fd, &active) != 0) {
		TRACE ("tcgetattr");
		return -1;
	}
	if (memcmp (&tty, &active, sizeof (struct termios) != 0)) {
		TRACE ("memcmp");
		return -1;
	}

	return 0;
}

//
// Configure the serial port (timeouts).
//

int
serial_set_timeout (serial *device, long timeout)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	device->timeout = timeout;

	return 0;
}


//
// Configure the serial port (recommended size of the input/output buffers).
//

int
serial_set_queue_size (serial *device, unsigned int input, unsigned int output)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	return 0;
}


#define MYSELECT(nfds, fds, queue, timeout) \
	(queue == SERIAL_QUEUE_INPUT ? \
	select (nfds, fds, NULL, NULL, timeout) : \
	select (nfds, NULL, fds, NULL, timeout))


static int
serial_poll_internal (int fd, int queue, long timeout, const struct timeval *timestamp)
{
	// Calculate the initial timeout, and obtain
	// a timestamp for updating the timeout.

	struct timeval tvt, tve;
	if (timeout > 0) {
		// Calculate the initial timeout.
		tvt.tv_sec  = (timeout / 1000);
		tvt.tv_usec = (timeout % 1000) * 1000;
		// Calculate the timestamp.
		if (timestamp == NULL) {
			struct timeval now;
			if (gettimeofday (&now, NULL) != 0) {
				TRACE ("gettimeofday");
				return -1;
			}
			timeradd (&now, &tvt, &tve);
		} else {
			timeradd (timestamp, &tvt, &tve);
		}
	} else if (timeout == 0) {
		timerclear (&tvt);
	}

	// Wait until the file descriptor is ready for reading/writing, or 
	// the timeout expires. A file descriptor is considered ready for 
	// reading/writing when a call to an input/output function with 
	// O_NONBLOCK clear would not block, whether or not the function 
	// would transfer data successfully. 

	fd_set fds;
	FD_ZERO (&fds);
	FD_SET (fd, &fds);

	int rc = 0;
	while ((rc = MYSELECT (fd + 1, &fds, queue, timeout >= 0 ? &tvt : NULL)) == -1) {
		if (errno != EINTR ) {
			TRACE ("select");
			return -1;
		}

		// According to the select() man pages, the file descriptor sets 
		// and the timeout become undefined after select() returns an error.
		// We follow the advise and do not rely on their contents.

		FD_ZERO (&fds);
		FD_SET (fd, &fds);

		// Calculate the remaining timeout.

		if (timeout > 0) {
			struct timeval now;
			if (gettimeofday (&now, NULL) != 0) {
				TRACE ("gettimeofday");
				return -1;
			}
			if (timercmp (&now, &tve, >=))
				timerclear (&tvt);
			else
				timersub (&tve, &now, &tvt);				
		} else if (timeout == 0) {
			timerclear (&tvt);
		}
	}

	return rc;
}


int
serial_read (serial* device, void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	long timeout = device->timeout;

	struct timeval timestamp;
	if (timeout > 0) {
		if (gettimeofday (&timestamp, NULL) != 0) {
			TRACE ("gettimeofday");
			return -1;
		}
	}

	unsigned int nbytes = 0;
	for (;;) {
		// Attempt to read data from the file descriptor.
		int n = read (device->fd, (char*) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR)
				continue; // Retry.
			if (errno != EAGAIN) {
				TRACE ("read");
				return -1; // Error during read call.
			}
		} else {
			nbytes += n;
			if (!n || nbytes == size)
				break; // Success or EOF.
		}

		// Wait until the file descriptor is ready for reading, or the timeout expires.
		int rc = serial_poll_internal (device->fd, SERIAL_QUEUE_INPUT, timeout, timeout > 0 ? &timestamp : NULL);
		if (rc < 0) {
			return -1; // Error during select/poll call.
		} else if (rc == 0)
			break; // Timeout.

		// Calculate the remaining timeout.
		if (timeout > 0) {
			struct timeval now, delta;
			if (gettimeofday (&now, NULL) != 0) {
				TRACE ("gettimeofday");
				return -1;
			}
			timersub (&now, &timestamp, &delta);
			long elapsed = delta.tv_sec * 1000 + delta.tv_usec / 1000;
			if (elapsed >= timeout)
				timeout = 0;
			else
				timeout -= elapsed;
			timestamp = now;
		}	
	}

	return nbytes;
}


int
serial_write (serial* device, const void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	unsigned int nbytes = 0;
	for (;;) {
		// Attempt to write data to the file descriptor.
		int n = write (device->fd, (char*) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR)
				continue; // Retry.
			if (errno != EAGAIN) {
				TRACE ("write");
				return -1; // Error during write call.
			}
		} else {
			nbytes += n;
			if (nbytes == size)
				break; // Success.
		}
		
		// Wait until the file descriptor is ready for writing, or the timeout expires.
		int rc = serial_poll_internal (device->fd, SERIAL_QUEUE_OUTPUT, -1, NULL);
		if (rc < 0) {
			return -1; // Error during select/poll call.
		} else if (rc == 0)
			break; // Timeout.
	}

	return nbytes;
}


int
serial_flush (serial *device, int queue)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int flags = 0;	

	switch (queue) {
	case SERIAL_QUEUE_INPUT:
		flags = TCIFLUSH;
		break;
	case SERIAL_QUEUE_OUTPUT:
		flags = TCOFLUSH;
		break;
	default:
		flags = TCIOFLUSH;
		break;
	}

	if (tcflush (device->fd, flags) != 0) {
		TRACE ("tcflush");
		return -1;
	}

	return 0;
}


int
serial_drain (serial *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	while (tcdrain (device->fd) != 0) {
		if (errno != EINTR ) {
			TRACE ("tcdrain");
			return -1;
		}
	}

	return 0;
}


int
serial_send_break (serial *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	if (tcsendbreak (device->fd, 0) != 0) {
		TRACE ("tcsendbreak");
		return -1;
	}
	
	return 0;
}


int
serial_set_break (serial *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int action = (level ? TIOCSBRK : TIOCCBRK);

	if (ioctl (device->fd, action, NULL) != 0) {
		TRACE ("ioctl");
		return -1;
	}

	return 0;
}


static int
serial_set_status (int fd, int value, int level)
{
	int action = (level ? TIOCMBIS : TIOCMBIC);

	if (ioctl (fd, action, &value) != 0) {
		TRACE ("ioctl");
		return -1;
	}

	return 0;
}


int
serial_set_dtr (serial *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	return serial_set_status (device->fd, TIOCM_DTR, level);
}


int
serial_set_rts (serial *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	return serial_set_status (device->fd, TIOCM_RTS, level);
}


int
serial_get_received (serial *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int bytes = 0;
	if (ioctl (device->fd, TIOCINQ, &bytes) != 0) {
		TRACE ("ioctl");
		return -1;
	}

	return bytes;
}


int
serial_get_transmitted (serial *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int bytes = 0;
	if (ioctl (device->fd, TIOCOUTQ, &bytes) != 0) {
		TRACE ("ioctl");
		return -1;
	}

	return bytes;
}


int
serial_sleep (unsigned long timeout)
{
	struct timespec ts;
	ts.tv_sec  = (timeout / 1000);
	ts.tv_nsec = (timeout % 1000) * 1000000;

	while (nanosleep (&ts, &ts) != 0) {
		if (errno != EINTR ) {
			TRACE ("nanosleep");
			return -1;
		}
	}

	return 0;
}


int
serial_timer (void)
{
	struct timeval tv;
	if (gettimeofday (&tv, NULL) != 0) {
		TRACE ("gettimeofday");
		return 0;
	}

	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
