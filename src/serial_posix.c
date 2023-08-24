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
#include <sys/select.h>	// select
#ifdef HAVE_LINUX_SERIAL_H
#include <linux/serial.h>
#endif
#ifdef HAVE_IOKIT_SERIAL_IOSS_H
#include <IOKit/serial/ioss.h>
#endif
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

#include <libdivecomputer/serial.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "platform.h"
#include "timer.h"

#define DIRNAME "/dev"

static dc_status_t dc_serial_iterator_next (dc_iterator_t *iterator, void *item);
static dc_status_t dc_serial_iterator_free (dc_iterator_t *iterator);

static dc_status_t dc_serial_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_serial_set_break (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_dtr (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_rts (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_get_lines (dc_iostream_t *iostream, unsigned int *value);
static dc_status_t dc_serial_get_available (dc_iostream_t *iostream, size_t *value);
static dc_status_t dc_serial_configure (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_serial_poll (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_serial_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);
static dc_status_t dc_serial_flush (dc_iostream_t *iostream);
static dc_status_t dc_serial_purge (dc_iostream_t *iostream, dc_direction_t direction);
static dc_status_t dc_serial_sleep (dc_iostream_t *iostream, unsigned int milliseconds);
static dc_status_t dc_serial_close (dc_iostream_t *iostream);

struct dc_serial_device_t {
	char name[256];
};

typedef struct dc_serial_iterator_t {
	dc_iterator_t base;
	dc_descriptor_t *descriptor;
	DIR *dp;
} dc_serial_iterator_t;

typedef struct dc_serial_t {
	dc_iostream_t base;
	/*
	 * The file descriptor corresponding to the serial port.
	 */
	int fd;
	int timeout;
	dc_timer_t *timer;
	/*
	 * Serial port settings are saved into this variable immediately
	 * after the port is opened. These settings are restored when the
	 * serial port is closed.
	 */
	struct termios tty;
} dc_serial_t;

static const dc_iterator_vtable_t dc_serial_iterator_vtable = {
	sizeof(dc_serial_iterator_t),
	dc_serial_iterator_next,
	dc_serial_iterator_free,
};

static const dc_iostream_vtable_t dc_serial_vtable = {
	sizeof(dc_serial_t),
	dc_serial_set_timeout, /* set_timeout */
	dc_serial_set_break, /* set_break */
	dc_serial_set_dtr, /* set_dtr */
	dc_serial_set_rts, /* set_rts */
	dc_serial_get_lines, /* get_lines */
	dc_serial_get_available, /* get_available */
	dc_serial_configure, /* configure */
	dc_serial_poll, /* poll */
	dc_serial_read, /* read */
	dc_serial_write, /* write */
	dc_serial_ioctl, /* ioctl */
	dc_serial_flush, /* flush */
	dc_serial_purge, /* purge */
	dc_serial_sleep, /* sleep */
	dc_serial_close, /* close */
};

static dc_status_t
syserror(int errcode)
{
	switch (errcode) {
	case EINVAL:
		return DC_STATUS_INVALIDARGS;
	case ENOMEM:
		return DC_STATUS_NOMEMORY;
	case ENOENT:
		return DC_STATUS_NODEVICE;
	case EACCES:
	case EBUSY:
		return DC_STATUS_NOACCESS;
	default:
		return DC_STATUS_IO;
	}
}

const char *
dc_serial_device_get_name (dc_serial_device_t *device)
{
	if (device == NULL || device->name[0] == '\0')
		return NULL;

	return device->name;
}

void
dc_serial_device_free (dc_serial_device_t *device)
{
	free (device);
}

dc_status_t
dc_serial_iterator_new (dc_iterator_t **out, dc_context_t *context, dc_descriptor_t *descriptor)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_serial_iterator_t *) dc_iterator_allocate (context, &dc_serial_iterator_vtable);
	if (iterator == NULL) {
		SYSERROR (context, ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	iterator->dp = opendir (DIRNAME);
	if (iterator->dp == NULL) {
		int errcode = errno;
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_free;
	}

	iterator->descriptor = descriptor;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;

error_free:
	dc_iterator_deallocate ((dc_iterator_t *) iterator);
	return status;
}

static dc_status_t
dc_serial_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_serial_iterator_t *iterator = (dc_serial_iterator_t *) abstract;
	dc_serial_device_t *device = NULL;

	struct dirent *ep = NULL;
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

	while ((ep = readdir (iterator->dp)) != NULL) {
		for (size_t i = 0; patterns[i] != NULL; ++i) {
			if (fnmatch (patterns[i], ep->d_name, 0) != 0)
				continue;

			char filename[sizeof(device->name)];
			int n = dc_platform_snprintf (filename, sizeof (filename), "%s/%s", DIRNAME, ep->d_name);
			if (n < 0 || (size_t) n >= sizeof (filename)) {
				return DC_STATUS_NOMEMORY;
			}

			if (!dc_descriptor_filter (iterator->descriptor, DC_TRANSPORT_SERIAL, filename)) {
				continue;
			}

			device = (dc_serial_device_t *) malloc (sizeof(dc_serial_device_t));
			if (device == NULL) {
				SYSERROR (abstract->context, ENOMEM);
				return DC_STATUS_NOMEMORY;
			}

			strncpy(device->name, filename, sizeof(device->name));

			*(dc_serial_device_t **) out = device;

			return DC_STATUS_SUCCESS;
		}
	}

	return DC_STATUS_DONE;
}

static dc_status_t
dc_serial_iterator_free (dc_iterator_t *abstract)
{
	dc_serial_iterator_t *iterator = (dc_serial_iterator_t *) abstract;

	closedir (iterator->dp);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_open (dc_iostream_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = NULL;

	if (out == NULL || name == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: name=%s", name);

	// Allocate memory.
	device = (dc_serial_t *) dc_iostream_allocate (context, &dc_serial_vtable, DC_TRANSPORT_SERIAL);
	if (device == NULL) {
		SYSERROR (context, ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Default to blocking reads.
	device->timeout = -1;

	// Create a high resolution timer.
	status = dc_timer_new (&device->timer);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to create a high resolution timer.");
		goto error_free;
	}

	// Open the device in non-blocking mode, to return immediately
	// without waiting for the modem connection to complete.
	device->fd = open (name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (device->fd == -1) {
		int errcode = errno;
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_timer_free;
	}

#ifndef ENABLE_PTY
	// Enable exclusive access mode.
	if (ioctl (device->fd, TIOCEXCL, NULL) != 0) {
		int errcode = errno;
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_close;
	}
#endif

	// Retrieve the current terminal attributes, to
	// be able to restore them when closing the device.
	// It is also used to check if the obtained
	// file descriptor represents a terminal device.
	if (tcgetattr (device->fd, &device->tty) != 0) {
		int errcode = errno;
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_close;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	close (device->fd);
error_timer_free:
	dc_timer_free (device->timer);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
}

static dc_status_t
dc_serial_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Restore the initial terminal attributes.
	if (tcsetattr (device->fd, TCSANOW, &device->tty) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

#ifndef ENABLE_PTY
	// Disable exclusive access mode.
	if (ioctl (device->fd, TIOCNXCL, NULL)) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}
#endif

	// Close the device.
	if (close (device->fd) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

	dc_timer_free (device->timer);

	return status;
}

static dc_status_t
dc_serial_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Retrieve the current settings.
	struct termios tty;
	memset (&tty, 0, sizeof (tty));
	if (tcgetattr (device->fd, &tty) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
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
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
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
#ifdef CMSPAR
	tty.c_cflag &= ~(PARENB | PARODD | CMSPAR);
#else
	tty.c_cflag &= ~(PARENB | PARODD);
#endif
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
#ifdef CMSPAR
	case DC_PARITY_MARK:
		tty.c_cflag |= (PARENB | PARODD | CMSPAR);
		tty.c_iflag |= INPCK;
		break;
	case DC_PARITY_SPACE:
		tty.c_cflag |= (PARENB | CMSPAR);
		tty.c_iflag |= INPCK;
		break;
#endif
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
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	// Configure a custom baudrate if necessary.
	if (custom) {
#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
		// Get the current settings.
		struct serial_struct ss;
		if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
			int errcode = errno;
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}

		// Set the custom divisor.
		ss.custom_divisor = ss.baud_base / baudrate;
		ss.flags &= ~ASYNC_SPD_MASK;
		ss.flags |= ASYNC_SPD_CUST;

		// Apply the new settings.
		if (ioctl (device->fd, TIOCSSERIAL, &ss) != 0 && NOPTY) {
			int errcode = errno;
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
#elif defined(IOSSIOSPEED)
		speed_t speed = baudrate;
		if (ioctl (device->fd, IOSSIOSPEED, &speed) != 0 && NOPTY) {
			int errcode = errno;
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
#else
		// Custom baudrates are not supported.
		return DC_STATUS_UNSUPPORTED;
#endif
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	device->timeout = timeout;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_latency (dc_iostream_t *abstract, unsigned int milliseconds)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

#if defined(TIOCGSERIAL) && defined(TIOCSSERIAL) && !defined(__ANDROID__)
	// Get the current settings.
	struct serial_struct ss;
	if (ioctl (device->fd, TIOCGSERIAL, &ss) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	// Set or clear the low latency flag.
	if (milliseconds == 0) {
		ss.flags |= ASYNC_LOW_LATENCY;
	} else {
		ss.flags &= ~ASYNC_LOW_LATENCY;
	}

	// Apply the new settings.
	if (ioctl (device->fd, TIOCSSERIAL, &ss) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}
#elif defined(IOSSDATALAT)
	// Set the receive latency in microseconds. Serial drivers use this
	// value to determine how often to dequeue characters received by
	// the hardware. A value of zero restores the default value.
	unsigned long usec = (milliseconds == 0 ? 1 : milliseconds * 1000);
	if (ioctl (device->fd, IOSSDATALAT, &usec) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}
#endif

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_poll (dc_iostream_t *abstract, int timeout)
{
	dc_serial_t *device = (dc_serial_t *) abstract;
	int rc = 0;

	do {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		struct timeval tv, *ptv = NULL;
		if (timeout > 0) {
			tv.tv_sec  = (timeout / 1000);
			tv.tv_usec = (timeout % 1000) * 1000;
			ptv = &tv;
		} else if (timeout == 0) {
			tv.tv_sec  = 0;
			tv.tv_usec = 0;
			ptv = &tv;
		}

		rc = select (device->fd + 1, &fds, NULL, NULL, ptv);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	} else if (rc == 0) {
		return DC_STATUS_TIMEOUT;
	} else {
		return DC_STATUS_SUCCESS;
	}
}

static dc_status_t
dc_serial_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	size_t nbytes = 0;

	// The absolute target time.
	dc_usecs_t target = 0;

	int init = 1;
	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		struct timeval tv, *ptv = NULL;
		if (device->timeout > 0) {
			dc_usecs_t timeout = 0;

			dc_usecs_t now = 0;
			status = dc_timer_now (device->timer, &now);
			if (status != DC_STATUS_SUCCESS) {
				goto out;
			}

			if (init) {
				// Calculate the initial timeout.
				timeout = (dc_usecs_t) device->timeout * 1000;
				// Calculate the target time.
				target = now + timeout;
				init = 0;
			} else {
				// Calculate the remaining timeout.
				if (now < target) {
					timeout = target - now;
				} else {
					timeout = 0;
				}
			}
			tv.tv_sec  = timeout / 1000000;
			tv.tv_usec = timeout % 1000000;
			ptv = &tv;
		} else if (device->timeout == 0) {
			tv.tv_sec  = 0;
			tv.tv_usec = 0;
			ptv = &tv;
		}

		int rc = select (device->fd + 1, &fds, NULL, NULL, ptv);
		if (rc < 0) {
			int errcode = errno;
			if (errcode == EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		ssize_t n = read (device->fd, (char *) data + nbytes, size - nbytes);
		if (n < 0) {
			int errcode = errno;
			if (errcode == EINTR || errcode == EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
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
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_serial_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		int rc = select (device->fd + 1, NULL, &fds, NULL, NULL);
		if (rc < 0) {
			int errcode = errno;
			if (errcode == EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		ssize_t n = write (device->fd, (const char *) data + nbytes, size - nbytes);
		if (n < 0) {
			int errcode = errno;
			if (errcode == EINTR || errcode == EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
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
		int errcode = errno;
		if (errcode != EINTR ) {
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		}
	}

out:
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_serial_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	switch (request) {
	case DC_IOCTL_SERIAL_SET_LATENCY:
		return dc_serial_set_latency (abstract, *(unsigned int *) data);
	default:
		return DC_STATUS_UNSUPPORTED;
	}
}

static dc_status_t
dc_serial_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

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
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_flush (dc_iostream_t *abstract)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_break (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	unsigned long action = (level ? TIOCSBRK : TIOCCBRK);

	if (ioctl (device->fd, action, NULL) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_dtr (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_DTR;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_rts (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	unsigned long action = (level ? TIOCMBIS : TIOCMBIC);

	int value = TIOCM_RTS;
	if (ioctl (device->fd, action, &value) != 0 && NOPTY) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	int bytes = 0;
	if (ioctl (device->fd, TIOCINQ, &bytes) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	if (value)
		*value = bytes;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;
	unsigned int lines = 0;

	int status = 0;
	if (ioctl (device->fd, TIOCMGET, &status) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
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

static dc_status_t
dc_serial_sleep (dc_iostream_t *abstract, unsigned int timeout)
{
	if (dc_platform_sleep (timeout) != 0) {
		int errcode = errno;
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}
