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
#include "context-private.h"
#include "array.h"

#ifdef _WIN32
#define ERRNO WSAGetLastError ()
#else
#define ERRNO errno
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

struct irda_t {
	dc_context_t *context;
#ifdef _WIN32
	SOCKET fd;
#else
	int fd;
#endif
	long timeout;
};


int
irda_socket_open (irda_t **out, dc_context_t *context)
{
	if (out == NULL)
		return -1; // EINVAL (Invalid argument)

	// Allocate memory.
	irda_t *device = (irda_t *) malloc (sizeof (irda_t));
	if (device == NULL) {
#ifdef _WIN32
		SYSERROR (context, ERROR_OUTOFMEMORY);
#else
		SYSERROR (context, errno);
#endif
		return -1; // ENOMEM (Not enough space)
	}

	// Library context.
	device->context = context;

	// Default to blocking reads.
	device->timeout = -1;

#ifdef _WIN32
	// Initialize the winsock dll.
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD (2, 2);
	if (WSAStartup (wVersionRequested, &wsaData) != 0) {
		SYSERROR (context, ERRNO);
		free (device);
		return -1;
	}

	// Confirm that the winsock dll supports version 2.2.
	// Note that if the dll supports versions greater than 2.2 in addition to
	// 2.2, it will still return 2.2 since that is the version we requested.
	if (LOBYTE (wsaData.wVersion) != 2 || 
		HIBYTE (wsaData.wVersion) != 2) {
		ERROR (context, "Incorrect winsock version.");
		WSACleanup ();
		free (device);
		return -1; 
	}
#endif

	// Open the socket.
	device->fd = socket (AF_IRDA, SOCK_STREAM, 0);
#ifdef _WIN32
	if (device->fd == INVALID_SOCKET) {
#else
	if (device->fd == -1) {
#endif
		SYSERROR (context, ERRNO);
#ifdef _WIN32
		WSACleanup ();
#endif
		free (device);
		return -1;
	}

	*out = device;

    return 0;
}


int
irda_socket_close (irda_t *device)
{
	if (device == NULL)
		return -1;

	// Terminate all send and receive operations.
	shutdown (device->fd, 0);

	// Close the socket.
#ifdef _WIN32
	if (closesocket (device->fd) != 0) {
#else
	if (close (device->fd) != 0) {
#endif
		SYSERROR (device->context, ERRNO);
#ifdef _WIN32
		WSACleanup ();
#endif
		free (device);
		return -1;
	}

#ifdef _WIN32
	// Terminate the winsock dll.
	if (WSACleanup () != 0) {
		SYSERROR (device->context, ERRNO);
		free (device);
		return -1;
	}
#endif

	// Free memory.	
	free (device);

	return 0;
}


int
irda_socket_set_timeout (irda_t *device, long timeout)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	device->timeout = timeout;

	return 0;
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

int
irda_socket_discover (irda_t *device, irda_callback_t callback, void *userdata)
{
	if (device == NULL)
		return -1;

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
#ifdef _WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
			if (errno != EAGAIN) {
#endif
				SYSERROR (device->context, ERRNO);
				return -1; // Error during getsockopt call.
			}
		}

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= DISCOVER_MAX_RETRIES)
			return 0;

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
			unsigned int address = array_uint32_be (list->Device[i].irdaDeviceID);
			unsigned int hints = (list->Device[i].irdaDeviceHints1 << 8) + 
									list->Device[i].irdaDeviceHints2;
			callback (address, 
				list->Device[i].irdaDeviceName,
				list->Device[i].irdaCharSet,
				hints, 
				userdata);
		}
#else
		for (unsigned int i = 0; i < list->len; ++i) {
			unsigned int hints = array_uint16_be (list->dev[i].hints);
			callback (list->dev[i].daddr, 
				list->dev[i].info, 
				list->dev[i].charset, 
				hints,
				userdata);
		}
#endif
	}

	return 0;
}


int
irda_socket_connect_name (irda_t *device, unsigned int address, const char *name)
{
	if (device == NULL)
		return -1;

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address >> 24) & 0xFF;
	peer.irdaDeviceID[1] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[2] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[3] = (address      ) & 0xFF;
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
		SYSERROR (device->context, ERRNO);
		return -1;
	}

	return 0;
}

int
irda_socket_connect_lsap (irda_t *device, unsigned int address, unsigned int lsap)
{
	if (device == NULL)
		return -1;

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address >> 24) & 0xFF;
	peer.irdaDeviceID[1] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[2] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[3] = (address      ) & 0xFF;
	snprintf (peer.irdaServiceName, 25, "LSAP-SEL%u", lsap);
#else
	struct sockaddr_irda peer;
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	peer.sir_lsap_sel = lsap;
	memset (peer.sir_name, 0x00, 25);
#endif

	if (connect (device->fd, (struct sockaddr *) &peer, sizeof (peer)) != 0) {
		SYSERROR (device->context, ERRNO);
		return -1;
	}

	return 0;
}


int
irda_socket_available (irda_t *device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

#ifdef _WIN32
	unsigned long bytes = 0;
	if (ioctlsocket (device->fd, FIONREAD, &bytes) != 0) {
#else
	int bytes = 0;
	if (ioctl (device->fd, FIONREAD, &bytes) != 0) {
#endif
		SYSERROR (device->context, ERRNO);
		return -1;
	}

	return bytes;
}


int
irda_socket_read (irda_t *device, void *data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	struct timeval tv;
	if (device->timeout >= 0) {
		tv.tv_sec  = (device->timeout / 1000);
		tv.tv_usec = (device->timeout % 1000) * 1000;
	}

	fd_set fds;
	FD_ZERO (&fds);
	FD_SET (device->fd, &fds);

	unsigned int nbytes = 0;
	while (nbytes < size) {
		int rc = select (device->fd + 1, &fds, NULL, NULL, (device->timeout >= 0 ? &tv : NULL));
		if (rc < 0) {
			SYSERROR (device->context, ERRNO);
			return -1; // Error during select call.
		} else if (rc == 0) {
			break; // Timeout.
		}

		int n = recv (device->fd, (char*) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			SYSERROR (device->context, ERRNO);
			return -1; // Error during recv call.
		} else if (n == 0) {
			break; // EOF reached.
		}

		nbytes += n;
	}

	return nbytes;
}


int
irda_socket_write (irda_t *device, const void *data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	unsigned int nbytes = 0;
	while (nbytes < size) {
		int n = send (device->fd, (char*) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			SYSERROR (device->context, ERRNO);
			return -1; // Error during send call.
		}

		nbytes += n;
	}

	return nbytes;
}
