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
#include "common-private.h"
#include "context-private.h"

struct dc_serial_t {
	/* Library context. */
	dc_context_t *context;
	/*
	 * The file descriptor corresponding to the serial port.
	 */
	int fd;
	int timeout;
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


dc_status_t
dc_serial_enumerate (dc_serial_callback_t callback, void *userdata)
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
		return DC_STATUS_IO;
	}

	while ((ep = readdir (dp)) != NULL) {
		for (size_t i = 0; patterns[i] != NULL; ++i) {
			if (fnmatch (patterns[i], ep->d_name, 0) == 0) {
				char filename[1024];
				int n = snprintf (filename, sizeof (filename), "%s/%s", dirname, ep->d_name);
				if (n >= sizeof (filename)) {
					closedir (dp);
					return DC_STATUS_NOMEMORY;
				}

				callback (filename, userdata);
				break;
			}
		}
	}

	closedir (dp);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_open (dc_serial_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: name=%s", name ? name : "");

	// Allocate memory.
	dc_serial_t *device = (dc_serial_t *) malloc (sizeof (dc_serial_t));
	if (device == NULL) {
		SYSERROR (context, ENOMEM);
		return DC_STATUS_NOMEMORY;
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
		status = DC_STATUS_IO;
		goto error_free;
	}

#ifndef ENABLE_PTY
	// Enable exclusive access mode.
	if (ioctl (device->fd, TIOCEXCL, NULL) != 0) {
		SYSERROR (context, errno);
		status = DC_STATUS_IO;
		goto error_close;
	}
#endif

	// Retrieve the current terminal attributes, to
	// be able to restore them when closing the device.
	// It is also used to check if the obtained
	// file descriptor represents a terminal device.
	if (tcgetattr (device->fd, &device->tty) != 0) {
		SYSERROR (context, errno);
		status = DC_STATUS_IO;
		goto error_close;
	}

	*out = device;

	return DC_STATUS_SUCCESS;

error_close:
	close (device->fd);
error_free:
	free (device);
	return status;
}

dc_status_t
dc_serial_close (dc_serial_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (device == NULL)
		return DC_STATUS_SUCCESS;

	// Restore the initial terminal attributes.
	if (tcsetattr (device->fd, TCSANOW, &device->tty) != 0) {
		SYSERROR (device->context, errno);
		dc_status_set_error(&status, DC_STATUS_IO);
	}

#ifndef ENABLE_PTY
	// Disable exclusive access mode.
	ioctl (device->fd, TIOCNXCL, NULL);
#endif

	// Close the device.
	if (close (device->fd) != 0) {
		SYSERROR (device->context, errno);
		dc_status_set_error(&status, DC_STATUS_IO);
	}

	// Free memory.
	free (device);

	return status;
}

dc_status_t
dc_serial_configure (dc_serial_t *device, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Configure: baudrate=%i, databits=%i, parity=%i, stopbits=%i, flowcontrol=%i",
		baudrate, databits, parity, stopbits, flowcontrol);

	// Retrieve the current settings.
	struct termios tty;
	memset (&tty, 0, sizeof (tty));
	if (tcgetattr (device->fd, &tty) != 0) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
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
		return DC_STATUS_IO;
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
		return DC_STATUS_INVALIDARGS;
	}

	// Set the parity type.
	tty.c_cflag &= ~(PARENB | PARODD);
	tty.c_iflag &= ~(IGNPAR | PARMRK | INPCK);
	switch (parity) {
	case DC_PARITY_NONE:
		tty.c_iflag |= IGNPAR;
		break;
	case DC_PARITY_EVEN:
		tty.c_cflag |= PARENB;
		tty.c_iflag |= INPCK;
		break;
	case DC_PARITY_ODD:
		tty.c_cflag |= (PARENB | PARODD);
		tty.c_iflag |= INPCK;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Set the number of stop bits.
	switch (stopbits) {
	case DC_STOPBITS_ONE:
		tty.c_cflag &= ~CSTOPB;
		break;
	case DC_STOPBITS_TWO:
		tty.c_cflag |= CSTOPB;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Set the flow control.
	switch (flowcontrol) {
	case DC_FLOWCONTROL_NONE:
		#ifdef CRTSCTS
			tty.c_cflag &= ~CRTSCTS;
		#endif
		tty.c_iflag &= ~(IXON | IXOFF | IXANY);
		break;
	case DC_FLOWCONTROL_HARDWARE:
		#ifdef CRTSCTS
			tty.c_cflag |= CRTSCTS;
			tty.c_iflag &= ~(IXON | IXOFF | IXANY);
			break;
		#else
			return DC_STATUS_UNSUPPORTED;
		#endif
	case DC_FLOWCONTROL_SOFTWARE:
		#ifdef CRTSCTS
			tty.c_cflag &= ~CRTSCTS;
		#endif
		tty.c_iflag |= (IXON | IXOFF);
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Apply the new settings.
	if (tcsetattr (device->fd, TCSANOW, &tty) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	// Configure a custom baudrate if necessary.
	if (custom) {
#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
		// Get the current settings.
		struct serial_struct ss;
		if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return DC_STATUS_IO;
		}

		// Set the custom divisor.
		ss.custom_divisor = ss.baud_base / baudrate;
		ss.flags &= ~ASYNC_SPD_MASK;
		ss.flags |= ASYNC_SPD_CUST;

		// Apply the new settings.
		if (ioctl (device->fd, TIOCSSERIAL, &ss) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return DC_STATUS_IO;
		}
#elif defined(IOSSIOSPEED)
		speed_t speed = baudrate;
		if (ioctl (device->fd, IOSSIOSPEED, &speed) != 0 && NOPTY) {
			SYSERROR (device->context, errno);
			return DC_STATUS_IO;
		}
#else
		// Custom baudrates are not supported.
		return DC_STATUS_UNSUPPORTED;
#endif
	}

	device->baudrate = baudrate;
	device->nbits = 1 + databits + stopbits + (parity ? 1 : 0);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_timeout (dc_serial_t *device, int timeout)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Timeout: value=%i", timeout);

	device->timeout = timeout;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_halfduplex (dc_serial_t *device, unsigned int value)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	device->halfduplex = value;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_latency (dc_serial_t *device, unsigned int milliseconds)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
	// Get the current settings.
	struct serial_struct ss;
	if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
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
		return DC_STATUS_IO;
	}
#elif defined(IOSSDATALAT)
	// Set the receive latency in microseconds. Serial drivers use this
	// value to determine how often to dequeue characters received by
	// the hardware. A value of zero restores the default value.
	unsigned long usec = (milliseconds == 0 ? 1 : milliseconds * 1000);
	if (ioctl (device->fd, IOSSDATALAT, &usec) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_read (dc_serial_t *device, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t nbytes = 0;

	if (device == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out;
	}

	// The total timeout.
	int timeout = device->timeout;

	// The absolute target time.
	struct timeval tve;

	int init = 1;
	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		struct timeval tvt;
		if (timeout > 0) {
			struct timeval now;
			if (gettimeofday (&now, NULL) != 0) {
				SYSERROR (device->context, errno);
				status = DC_STATUS_IO;
				goto out;
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
			status = DC_STATUS_IO;
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		ssize_t n = read (device->fd, (char *) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue; // Retry.
			SYSERROR (device->context, errno);
			status = DC_STATUS_IO;
			goto out;
		} else if (n == 0) {
			 break; // EOF.
		}

		nbytes += n;
	}

	if (nbytes != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Read", (unsigned char *) data, nbytes);

	if (actual)
		*actual = nbytes;

	return status;
}

dc_status_t
dc_serial_write (dc_serial_t *device, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t nbytes = 0;

	if (device == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out;
	}

	struct timeval tve, tvb;
	if (device->halfduplex) {
		// Get the current time.
		if (gettimeofday (&tvb, NULL) != 0) {
			SYSERROR (device->context, errno);
			status = DC_STATUS_IO;
			goto out;
		}
	}

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		int rc = select (device->fd + 1, NULL, &fds, NULL, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue; // Retry.
			SYSERROR (device->context, errno);
			status = DC_STATUS_IO;
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		ssize_t n = write (device->fd, (const char *) data + nbytes, size - nbytes);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue; // Retry.
			SYSERROR (device->context, errno);
			status = DC_STATUS_IO;
			goto out;
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
			status = DC_STATUS_IO;
			goto out;
		}
	}

	if (device->halfduplex) {
		// Get the current time.
		if (gettimeofday (&tve, NULL) != 0) {
			SYSERROR (device->context, errno);
			status = DC_STATUS_IO;
			goto out;
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
			dc_serial_sleep (device, (remaining + 999) / 1000);
		}
	}

out:
	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Write", (unsigned char *) data, nbytes);

	if (actual)
		*actual = nbytes;

	return status;
}

dc_status_t
dc_serial_purge (dc_serial_t *device, dc_direction_t direction)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Purge: direction=%u", direction);

	int flags = 0;

	switch (direction) {
	case DC_DIRECTION_INPUT:
		flags = TCIFLUSH;
		break;
	case DC_DIRECTION_OUTPUT:
		flags = TCOFLUSH;
		break;
	case DC_DIRECTION_ALL:
		flags = TCIOFLUSH;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	if (tcflush (device->fd, flags) != 0) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_flush (dc_serial_t *device)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Flush: none");

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_break (dc_serial_t *device, unsigned int level)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Break: value=%i", level);

	unsigned long action = (level ? TIOCSBRK : TIOCCBRK);

	if (ioctl (device->fd, action, NULL) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_dtr (dc_serial_t *device, unsigned int level)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "DTR: value=%i", level);

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_DTR;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_set_rts (dc_serial_t *device, unsigned int level)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "RTS: value=%i", level);

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_RTS;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_get_available (dc_serial_t *device, size_t *value)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	int bytes = 0;
	if (ioctl (device->fd, TIOCINQ, &bytes) != 0) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	if (value)
		*value = bytes;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_get_lines (dc_serial_t *device, unsigned int *value)
{
	unsigned int lines = 0;

	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	int status = 0;
	if (ioctl (device->fd, TIOCMGET, &status) != 0) {
		SYSERROR (device->context, errno);
		return DC_STATUS_IO;
	}

	if (status & TIOCM_CAR)
		lines |= DC_LINE_DCD;
	if (status & TIOCM_CTS)
		lines |= DC_LINE_CTS;
	if (status & TIOCM_DSR)
		lines |= DC_LINE_DSR;
	if (status & TIOCM_RNG)
		lines |= DC_LINE_RNG;

	if (value)
		*value = lines;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_sleep (dc_serial_t *device, unsigned int timeout)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Sleep: value=%u", timeout);

	struct timespec ts;
	ts.tv_sec  = (timeout / 1000);
	ts.tv_nsec = (timeout % 1000) * 1000000;

	while (nanosleep (&ts, &ts) != 0) {
		if (errno != EINTR ) {
			SYSERROR (device->context, errno);
			return DC_STATUS_IO;
		}
	}

	return DC_STATUS_SUCCESS;
}
