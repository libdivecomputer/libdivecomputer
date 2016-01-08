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
#include <stdio.h>	// snprintf
#ifdef _WIN32
	#define NOGDI
	#include <winsock2.h>
	#include <windows.h>
	#include <af_irda.h>
#else
	#include <string.h>			// strerror
	#include <errno.h>			// errno
	#include <unistd.h>			// close
	#include <sys/types.h>		// socket, getsockopt
	#include <sys/socket.h>		// socket, getsockopt
	#include <linux/types.h>	// irda
	#include <linux/irda.h>		// irda
	#include <sys/select.h>		// select
	#include <sys/ioctl.h>		// ioctl
#endif

#include "irda.h"
#include "common-private.h"
#include "context-private.h"
#include "array.h"

#ifdef _WIN32
typedef int s_ssize_t;
typedef DWORD s_errcode_t;
#define S_ERRNO WSAGetLastError ()
#define S_EAGAIN WSAEWOULDBLOCK
#define S_ENOMEM WSA_NOT_ENOUGH_MEMORY
#define S_EINVAL WSAEINVAL
#define S_EACCES WSAEACCES
#define S_EAFNOSUPPORT WSAEAFNOSUPPORT
#define S_INVALID INVALID_SOCKET
#define S_IOCTL ioctlsocket
#define S_CLOSE closesocket
#else
typedef ssize_t s_ssize_t;
typedef int s_errcode_t;
#define S_ERRNO errno
#define S_EAGAIN EAGAIN
#define S_ENOMEM ENOMEM
#define S_EINVAL EINVAL
#define S_EACCES EACCES
#define S_EAFNOSUPPORT EAFNOSUPPORT
#define S_INVALID -1
#define S_IOCTL ioctl
#define S_CLOSE close
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

struct dc_irda_t {
	dc_context_t *context;
#ifdef _WIN32
	SOCKET fd;
#else
	int fd;
#endif
	int timeout;
};

static dc_status_t
syserror(s_errcode_t errcode)
{
	switch (errcode) {
	case S_EINVAL:
		return DC_STATUS_INVALIDARGS;
	case S_ENOMEM:
		return DC_STATUS_NOMEMORY;
	case S_EACCES:
		return DC_STATUS_NOACCESS;
	case S_EAFNOSUPPORT:
		return DC_STATUS_UNSUPPORTED;
	default:
		return DC_STATUS_IO;
	}
}

dc_status_t
dc_irda_open (dc_irda_t **out, dc_context_t *context)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_irda_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (dc_irda_t *) malloc (sizeof (dc_irda_t));
	if (device == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Library context.
	device->context = context;

	// Default to blocking reads.
	device->timeout = -1;

#ifdef _WIN32
	// Initialize the winsock dll.
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD (2, 2);
	int rc = WSAStartup (wVersionRequested, &wsaData);
	if (rc != 0) {
		SYSERROR (context, rc);
		status = DC_STATUS_UNSUPPORTED;
		goto error_free;
	}

	// Confirm that the winsock dll supports version 2.2.
	// Note that if the dll supports versions greater than 2.2 in addition to
	// 2.2, it will still return 2.2 since that is the version we requested.
	if (LOBYTE (wsaData.wVersion) != 2 ||
		HIBYTE (wsaData.wVersion) != 2) {
		ERROR (context, "Incorrect winsock version.");
		status = DC_STATUS_UNSUPPORTED;
		goto error_wsacleanup;
	}
#endif

	// Open the socket.
	device->fd = socket (AF_IRDA, SOCK_STREAM, 0);
	if (device->fd == S_INVALID) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (context, errcode);
		status = syserror(errcode);
		goto error_wsacleanup;
	}

	*out = device;

    return DC_STATUS_SUCCESS;

error_wsacleanup:
#ifdef _WIN32
	WSACleanup ();
error_free:
#endif
	free (device);
	return status;
}

dc_status_t
dc_irda_close (dc_irda_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (device == NULL)
		return DC_STATUS_SUCCESS;

	// Terminate all send and receive operations.
	shutdown (device->fd, 0);

	// Close the socket.
	if (S_CLOSE (device->fd) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (device->context, errcode);
		dc_status_set_error(&status, syserror(errcode));
	}

#ifdef _WIN32
	// Terminate the winsock dll.
	if (WSACleanup () != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (device->context, errcode);
		dc_status_set_error(&status, syserror(errcode));
	}
#endif

	// Free memory.
	free (device);

	return status;
}

dc_status_t
dc_irda_set_timeout (dc_irda_t *device, int timeout)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Timeout: value=%i", timeout);

	device->timeout = timeout;

	return DC_STATUS_SUCCESS;
}


#define DISCOVER_MAX_DEVICES 16	// Maximum number of devices.
#define DISCOVER_MAX_RETRIES 4	// Maximum number of retries.

#ifdef _WIN32
#define DISCOVER_BUFSIZE sizeof (DEVICELIST) + \
				sizeof (IRDA_DEVICE_INFO) * (DISCOVER_MAX_DEVICES - 1)
#else
#define DISCOVER_BUFSIZE sizeof (struct irda_device_list) + \
				sizeof (struct irda_device_info) * (DISCOVER_MAX_DEVICES - 1)
#endif

dc_status_t
dc_irda_discover (dc_irda_t *device, dc_irda_callback_t callback, void *userdata)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	unsigned char data[DISCOVER_BUFSIZE] = {0};
#ifdef _WIN32
	DEVICELIST *list = (DEVICELIST *) data;
	int size = sizeof (data);
#else
	struct irda_device_list *list = (struct irda_device_list *) data;
	socklen_t size = sizeof (data);
#endif

	int rc = 0;
	unsigned int nretries = 0;
	while ((rc = getsockopt (device->fd, SOL_IRLMP, IRLMP_ENUMDEVICES, (char*) data, &size)) != 0 ||
#ifdef _WIN32
		list->numDevice == 0)
#else
		list->len == 0)
#endif
	{
		// Automatically retry the discovery when no devices were found.
		// On Linux, getsockopt fails with EAGAIN when no devices are
		// discovered, while on Windows it succeeds and sets the number
		// of devices to zero. Both situations are handled the same here.
		if (rc != 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode != S_EAGAIN) {
				SYSERROR (device->context, errcode);
				return syserror(errcode);
			}
		}

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= DISCOVER_MAX_RETRIES)
			return DC_STATUS_SUCCESS;

		// Restore the size parameter in case it was
		// modified by the previous getsockopt call.
		size = sizeof (data);

#ifdef _WIN32
		Sleep (1000);
#else
		sleep (1);
#endif
	}

	if (callback) {
#ifdef _WIN32
		for (unsigned int i = 0; i < list->numDevice; ++i) {
			const char *name = list->Device[i].irdaDeviceName;
			unsigned int address = array_uint32_le (list->Device[i].irdaDeviceID);
			unsigned int charset = list->Device[i].irdaCharSet;
			unsigned int hints = (list->Device[i].irdaDeviceHints1 << 8) +
									list->Device[i].irdaDeviceHints2;
#else
		for (unsigned int i = 0; i < list->len; ++i) {
			const char *name = list->dev[i].info;
			unsigned int address = list->dev[i].daddr;
			unsigned int charset = list->dev[i].charset;
			unsigned int hints = array_uint16_be (list->dev[i].hints);
#endif

			INFO (device->context,
				"Discover: address=%08x, name=%s, charset=%02x, hints=%04x",
				address, name, charset, hints);

			callback (address, name, charset, hints, userdata);
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_irda_connect_name (dc_irda_t *device, unsigned int address, const char *name)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Connect: address=%08x, name=%s", address, name ? name : "");

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address      ) & 0xFF;
	peer.irdaDeviceID[1] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[2] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[3] = (address >> 24) & 0xFF;
    if (name)
		strncpy (peer.irdaServiceName, name, 25);
	else
		memset (peer.irdaServiceName, 0x00, 25);
#else
	struct sockaddr_irda peer;
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	if (name)
		strncpy (peer.sir_name, name, 25);
	else
		memset (peer.sir_name, 0x00, 25);
#endif

	if (connect (device->fd, (struct sockaddr *) &peer, sizeof (peer)) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (device->context, errcode);
		return syserror(errcode);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_irda_connect_lsap (dc_irda_t *device, unsigned int address, unsigned int lsap)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (device->context, "Connect: address=%08x, lsap=%u", address, lsap);

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address      ) & 0xFF;
	peer.irdaDeviceID[1] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[2] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[3] = (address >> 24) & 0xFF;
	snprintf (peer.irdaServiceName, 25, "LSAP-SEL%u", lsap);
#else
	struct sockaddr_irda peer;
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	peer.sir_lsap_sel = lsap;
	memset (peer.sir_name, 0x00, 25);
#endif

	if (connect (device->fd, (struct sockaddr *) &peer, sizeof (peer)) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (device->context, errcode);
		return syserror(errcode);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_irda_get_available (dc_irda_t *device, size_t *value)
{
	if (device == NULL)
		return DC_STATUS_INVALIDARGS;

#ifdef _WIN32
	unsigned long bytes = 0;
#else
	int bytes = 0;
#endif

	if (S_IOCTL (device->fd, FIONREAD, &bytes) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (device->context, errcode);
		return syserror(errcode);
	}

	if (value)
		*value = bytes;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_irda_read (dc_irda_t *device, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t nbytes = 0;

	if (device == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out;
	}

	struct timeval tv;
	if (device->timeout >= 0) {
		tv.tv_sec  = (device->timeout / 1000);
		tv.tv_usec = (device->timeout % 1000) * 1000;
	}

	fd_set fds;
	FD_ZERO (&fds);
	FD_SET (device->fd, &fds);

	while (nbytes < size) {
		int rc = select (device->fd + 1, &fds, NULL, NULL, (device->timeout >= 0 ? &tv : NULL));
		if (rc < 0) {
			s_errcode_t errcode = S_ERRNO;
			SYSERROR (device->context, errcode);
			status = syserror(errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		s_ssize_t n = recv (device->fd, (char*) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			SYSERROR (device->context, errcode);
			status = syserror(errcode);
			goto out;
		} else if (n == 0) {
			break; // EOF reached.
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
dc_irda_write (dc_irda_t *device, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t nbytes = 0;

	if (device == NULL) {
		status = DC_STATUS_INVALIDARGS;
		goto out;
	}

	while (nbytes < size) {
		s_ssize_t n = send (device->fd, (char*) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			SYSERROR (device->context, errcode);
			status = syserror(errcode);
			goto out;
		}

		nbytes += n;
	}

	if (nbytes != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Write", (unsigned char *) data, nbytes);

	if (actual)
		*actual = nbytes;

	return status;
}
