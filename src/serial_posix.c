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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h> // malloc, free
#include <string.h>	// strerror
#include <errno.h>	// errno
#include <unistd.h>	// open, close, read, write
#include <fcntl.h>	// fcntl
#include <termios.h>	// tcgetattr, tcsetattr, cfsetispeed, cfsetospeed, tcflush, tcsendbreak
#include <sys/ioctl.h>	// ioctl
#include <sys/time.h>	// gettimeofday
#include <time.h>	// nanosleep
#ifdef HAVE_LINUX_SERIAL_H
#include <linux/serial.h>
#endif
#ifdef HAVE_IOKIT_SERIAL_IOSS_H
#include <IOKit/serial/ioss.h>
#endif
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <fnmatch.h>

#ifndef TIOCINQ
#define TIOCINQ FIONREAD
#endif

#ifdef ENABLE_PTY
#define NOPTY (errno != EINVAL && errno != ENOTTY)
#else
#define NOPTY 1
#endif

#include "serial.h"
#include "context-private.h"

struct serial_t {
	/* Library context. */
	dc_context_t *context;
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
	/* Half-duplex settings */
	int halfduplex;
	unsigned int baudrate;
	unsigned int nbits;
};


int
serial_enumerate (serial_callback_t callback, void *userdata)
{
	DIR *dp = NULL;
	struct dirent *ep = NULL;
	const char *dirname = "/dev";
	const char *patterns[] = {
#if defined (__APPLE__)
		"tty.*",
#else
		"ttyS*",
		"ttyUSB*",
		"ttyACM*",
		"rfcomm*",
#endif
		NULL
	};

	dp = opendir (dirname);
	if (dp == NULL) {
		return -1;
	}

	while ((ep = readdir (dp)) != NULL) {
		for (size_t i = 0; patterns[i] != NULL; ++i) {
			if (fnmatch (patterns[i], ep->d_name, 0) == 0) {
				char filename[1024];
				int n = snprintf (filename, sizeof (filename), "%s/%s", dirname, ep->d_name);
				if (n >= sizeof (filename)) {
					closedir (dp);
					return -1;
				}

				callback (filename, userdata);
				break;
			}
		}
	}

	closedir (dp);

	return 0;
}


//
// Open the serial port.
//

int
serial_open (serial_t **out, dc_context_t *context, const char* name)
{
	if (out == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (context, "Open: name=%s", name ? name : "");

	// Allocate memory.
	serial_t *device = (serial_t *) malloc (sizeof (serial_t));
	if (device == NULL) {
		SYSERROR (context, errno);
		return -1; // ENOMEM (Not enough space)
	}

	// Library context.
	device->context = context;

	// Default to blocking reads.
	device->timeout = -1;

	// Default to full-duplex.
	device->halfduplex = 0;
	device->baudrate = 0;
	device->nbits = 0;

	// Open the device in non-blocking mode, to return immediately
	// without waiting for the modem connection to complete.
	device->fd = open (name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (device->fd == -1) {
		SYSERROR (context, errno);
		free (device);
		return -1; // Error during open call.
	}

#ifndef ENABLE_PTY
	// Enable exclusive access mode.
	if (ioctl (device->fd, TIOCEXCL, NULL) != 0) {
		SYSERROR (context, errno);
		close (device->fd);
		free (device);
		return -1;
	}
#endif

	// Retrieve the current terminal attributes, to
	// be able to restore them when closing the device.
	// It is also used to check if the obtained
	// file descriptor represents a terminal device.
	if (tcgetattr (device->fd, &device->tty) != 0) {
		SYSERROR (context, errno);
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
serial_close (serial_t *device)
{
	if (device == NULL)
		return 0;

	// Restore the initial terminal attributes.
	if (tcsetattr (device->fd, TCSANOW, &device->tty) != 0) {
		SYSERROR (device->context, errno);
		close (device->fd);
		free (device);
		return -1;
	}

	// Close the device.
	if (close (device->fd) != 0) {
		SYSERROR (device->context, errno);
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
serial_configure (serial_t *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "Configure: baudrate=%i, databits=%i, parity=%i, stopbits=%i, flowcontrol=%i",
		baudrate, databits, parity, stopbits, flowcontrol);

	// Retrieve the current settings.
	struct termios tty;
	if (tcgetattr (device->fd, &tty) != 0) {
		SYSERROR (device->context, errno);
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
	int custom = 0;
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
#ifdef B460800
	case 460800: baud = B460800; break;
#endif
#ifdef B500000
	case 500000: baud = B500000; break;
#endif
#ifdef B576000
	case 576000: baud = B576000; break;
#endif
#ifdef B921600
	case 921600: baud = B921600; break;
#endif
#ifdef B1000000
	case 1000000: baud = B1000000; break;
#endif
#ifdef B1152000
	case 1152000: baud = B1152000; break;
#endif
#ifdef B1500000
	case 1500000: baud = B1500000; break;
#endif
#ifdef B2000000
	case 2000000: baud = B2000000; break;
#endif
#ifdef B2500000
	case 2500000: baud = B2500000; break;
#endif
#ifdef B3000000
	case 3000000: baud = B3000000; break;
#endif
#ifdef B3500000
	case 3500000: baud = B3500000; break;
#endif
#ifdef B4000000
	case 4000000: baud = B4000000; break;
#endif
	default:
		baud = B38400; /* Required for custom baudrates on linux. */
		custom = 1;
		break;
	}
	if (cfsetispeed (&tty, baud) != 0 ||
		cfsetospeed (&tty, baud) != 0) {
		SYSERROR (device->context, errno);
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
		SYSERROR (device->context, errno);
		return -1;
	}

	// tcsetattr() returns success if any of the requested changes could be
	// successfully carried out. Therefore, when making multiple changes
	// it may be necessary to follow this call with a further call to
	// tcgetattr() to check that all changes have been performed successfully.

	struct termios active;
	if (tcgetattr (device->fd, &active) != 0) {
		SYSERROR (device->context, errno);
		return -1;
	}
	if (memcmp (&tty, &active, sizeof (struct termios) != 0)) {
		ERROR (device->context, "Failed to set the terminal attributes.");
		return -1;
	}

	// Configure a custom baudrate if necessary.
	if (custom) {
#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
		// Get the current settings.
		struct serial_struct ss;
		if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return -1;
		}

		// Set the custom divisor.
		ss.custom_divisor = ss.baud_base / baudrate;
		ss.flags &= ~ASYNC_SPD_MASK;
		ss.flags |= ASYNC_SPD_CUST;

		// Apply the new settings.
		if (ioctl (device->fd, TIOCSSERIAL, &ss) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return -1;
		}
#elif defined(IOSSIOSPEED)
		speed_t speed = baudrate;
		if (ioctl (device->fd, IOSSIOSPEED, &speed) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return -1;
		}
#else
		// Custom baudrates are not supported.
		return -1;
#endif
	}

	device->baudrate = baudrate;
	device->nbits = 1 + databits + stopbits + (parity ? 1 : 0);

	return 0;
}

//
// Configure the serial port (timeouts).
//

int
serial_set_timeout (serial_t *device, long timeout)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "Timeout: value=%li", timeout);

	device->timeout = timeout;

	return 0;
}


//
// Configure the serial port (recommended size of the input/output buffers).
//

int
serial_set_queue_size (serial_t *device, unsigned int input, unsigned int output)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	return 0;
}


int
serial_set_halfduplex (serial_t *device, int value)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	device->halfduplex = value;

	return 0;
}

int
serial_set_latency (serial_t *device, unsigned int milliseconds)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
	// Get the current settings.
	struct serial_struct ss;
	if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}

	// Set or clear the low latency flag.
	if (milliseconds == 0) {
		ss.flags |= ASYNC_LOW_LATENCY;
	} else {
		ss.flags &= ~ASYNC_LOW_LATENCY;
	}

	// Apply the new settings.
	if (ioctl (device->fd, TIOCSSERIAL, &ss) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}
#elif defined(IOSSDATALAT)
	// Set the receive latency in microseconds. Serial drivers use this
	// value to determine how often to dequeue characters received by
	// the hardware. A value of zero restores the default value.
	unsigned long usec = (milliseconds == 0 ? 1 : milliseconds * 1000);
	if (ioctl (device->fd, IOSSDATALAT, &usec) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}
#endif

	return 0;
}

int
serial_read (serial_t *device, void *data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	// The total timeout.
	long timeout = device->timeout;

	// The absolute target time.
	struct timeval tve;

	int init = 1;
	unsigned int nbytes = 0;
	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		struct timeval tvt;
		if (timeout > 0) {
			struct timeval now;
			if (gettimeofday (&now, NULL) != 0) {
				SYSERROR (device->context, errno);
				return -1;
			}

			if (init) {
				// Calculate the initial timeout.
				tvt.tv_sec  = (timeout / 1000);
				tvt.tv_usec = (timeout % 1000) * 1000;
				// Calculate the target time.
				timeradd (&now, &tvt, &tve);
			} else {
				// Calculate the remaining timeout.
				if (timercmp (&now, &tve, <))
					timersub (&tve, &now, &tvt);
				else
					timerclear (&tvt);
			}
			init = 0;
		} else if (timeout == 0) {
			timerclear (&tvt);
		}

		int rc = select (device->fd + 1, &fds, NULL, NULL, timeout >= 0 ? &tvt : NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue; // Retry.
			SYSERROR (device->context, errno);
			return -1; // Error during select call.
		} else if (rc == 0) {
			break; // Timeout.
		}

		int n = read (device->fd, (char *) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue; // Retry.
			SYSERROR (device->context, errno);
			return -1; // Error during read call.
		} else if (n == 0) {
			 break; // EOF.
		}

		nbytes += n;
	}

	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Read", (unsigned char *) data, nbytes);

	return nbytes;
}


int
serial_write (serial_t *device, const void *data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	struct timeval tve, tvb;
	if (device->halfduplex) {
		// Get the current time.
		if (gettimeofday (&tvb, NULL) != 0) {
			SYSERROR (device->context, errno);
			return -1;
		}
	}

	unsigned int nbytes = 0;
	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		int rc = select (device->fd + 1, NULL, &fds, NULL, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue; // Retry.
			SYSERROR (device->context, errno);
			return -1; // Error during select call.
		} else if (rc == 0) {
			break; // Timeout.
		}

		int n = write (device->fd, (char *) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue; // Retry.
			SYSERROR (device->context, errno);
			return -1; // Error during write call.
		} else if (n == 0) {
			 break; // EOF.
		}

		nbytes += n;
	}

	// Wait until all data has been transmitted.
#ifdef __ANDROID__
	/* Android is missing tcdrain, so use ioctl version instead */
	while (ioctl (device->fd, TCSBRK, 1) != 0) {
#else
	while (tcdrain (device->fd) != 0) {
#endif
		if (errno != EINTR ) {
			SYSERROR (device->context, errno);
			return -1;
		}
	}

	if (device->halfduplex) {
		// Get the current time.
		if (gettimeofday (&tve, NULL) != 0) {
			SYSERROR (device->context, errno);
			return -1;
		}

		// Calculate the elapsed time (microseconds).
		struct timeval tvt;
		timersub (&tve, &tvb, &tvt);
		unsigned long elapsed = tvt.tv_sec * 1000000 + tvt.tv_usec;

		// Calculate the expected duration (microseconds). A 2 millisecond fudge
		// factor is added because it improves the success rate significantly.
		unsigned long expected = 1000000.0 * device->nbits / device->baudrate * size + 0.5 + 2000;

		// Wait for the remaining time.
		if (elapsed < expected) {
			unsigned long remaining = expected - elapsed;

			// The remaining time is rounded up to the nearest millisecond to
			// match the Windows implementation. The higher resolution is
			// pointless anyway, since we already added a fudge factor above.
			serial_sleep (device, (remaining + 999) / 1000);
		}
	}

	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Write", (unsigned char *) data, nbytes);

	return nbytes;
}


int
serial_flush (serial_t *device, int queue)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "Flush: queue=%u, input=%i, output=%i", queue,
		serial_get_received (device),
		serial_get_transmitted (device));

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
		SYSERROR (device->context, errno);
		return -1;
	}

	return 0;
}


int
serial_send_break (serial_t *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	if (tcsendbreak (device->fd, 0) != 0) {
		SYSERROR (device->context, errno);
		return -1;
	}
	
	return 0;
}


int
serial_set_break (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "Break: value=%i", level);

	unsigned long action = (level ? TIOCSBRK : TIOCCBRK);

	if (ioctl (device->fd, action, NULL) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}

	return 0;
}


int
serial_set_dtr (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "DTR: value=%i", level);

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_DTR;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}

	return 0;
}


int
serial_set_rts (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	INFO (device->context, "RTS: value=%i", level);

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_RTS;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return -1;
	}

	return 0;
}


int
serial_get_received (serial_t *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int bytes = 0;
	if (ioctl (device->fd, TIOCINQ, &bytes) != 0) {
		SYSERROR (device->context, errno);
		return -1;
	}

	return bytes;
}


int
serial_get_transmitted (serial_t *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int bytes = 0;
	if (ioctl (device->fd, TIOCOUTQ, &bytes) != 0) {
		SYSERROR (device->context, errno);
		return -1;
	}

	return bytes;
}


int
serial_get_line (serial_t *device, int line)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	int status = 0;
	if (ioctl (device->fd, TIOCMGET, &status) != 0) {
		SYSERROR (device->context, errno);
		return -1;
	}

	switch (line) {
	case SERIAL_LINE_DCD:
		return (status & TIOCM_CAR) == TIOCM_CAR;
	case SERIAL_LINE_CTS:
		return (status & TIOCM_CTS) == TIOCM_CTS;
	case SERIAL_LINE_DSR:
		return (status & TIOCM_DSR) == TIOCM_DSR;
	case SERIAL_LINE_RNG:
		return (status & TIOCM_RNG) == TIOCM_RNG;
	default:
		return -1;
	}

	return 0;
}


int
serial_sleep (serial_t *device, unsigned long timeout)
{
	if (device == NULL)
		return -1;

	INFO (device->context, "Sleep: value=%lu", timeout);

	struct timespec ts;
	ts.tv_sec  = (timeout / 1000);
	ts.tv_nsec = (timeout % 1000) * 1000000;

	while (nanosleep (&ts, &ts) != 0) {
		if (errno != EINTR ) {
			SYSERROR (device->context, errno);
			return -1;
		}
	}

	return 0;
}
