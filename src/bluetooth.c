/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#ifdef _WIN32
#define NOGDI
#include <winsock2.h>
#include <windows.h>
#ifdef HAVE_WS2BTH_H
#define BLUETOOTH
#include <ws2bth.h>
#endif
#else
#include <errno.h>      // errno
#include <unistd.h>     // close
#include <sys/types.h>  // socket, getsockopt
#include <sys/socket.h> // socket, getsockopt
#include <sys/select.h> // select
#include <sys/ioctl.h>  // ioctl
#include <sys/time.h>
#ifdef HAVE_BLUEZ
#define BLUETOOTH
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#endif
#endif

#include "bluetooth.h"

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"

#ifdef _WIN32
typedef int s_ssize_t;
typedef DWORD s_errcode_t;
#define S_ERRNO WSAGetLastError ()
#define S_EINTR WSAEINTR
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
#define S_EINTR EINTR
#define S_EAGAIN EAGAIN
#define S_ENOMEM ENOMEM
#define S_EINVAL EINVAL
#define S_EACCES EACCES
#define S_EAFNOSUPPORT EAFNOSUPPORT
#define S_INVALID -1
#define S_IOCTL ioctl
#define S_CLOSE close
#endif

#ifdef _WIN32
#define DC_ADDRESS_FORMAT "%012I64X"
#else
#define DC_ADDRESS_FORMAT "%012llX"
#endif

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

#define MAX_DEVICES 255
#define MAX_PERIODS 8

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_bluetooth_vtable)

#ifdef BLUETOOTH
static dc_status_t dc_bluetooth_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_bluetooth_get_available (dc_iostream_t *iostream, size_t *value);
static dc_status_t dc_bluetooth_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_bluetooth_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_bluetooth_close (dc_iostream_t *iostream);

typedef struct dc_bluetooth_t {
	dc_iostream_t base;
#ifdef _WIN32
	SOCKET fd;
#else
	int fd;
#endif
	int timeout;
} dc_bluetooth_t;

static const dc_iostream_vtable_t dc_bluetooth_vtable = {
	sizeof(dc_bluetooth_t),
	dc_bluetooth_set_timeout, /* set_timeout */
	NULL, /* set_latency */
	NULL, /* set_halfduplex */
	NULL, /* set_break */
	NULL, /* set_dtr */
	NULL, /* set_rts */
	NULL, /* get_lines */
	dc_bluetooth_get_available, /* get_received */
	NULL, /* configure */
	dc_bluetooth_read, /* read */
	dc_bluetooth_write, /* write */
	NULL, /* flush */
	NULL, /* purge */
	NULL, /* sleep */
	dc_bluetooth_close, /* close */
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

#ifdef HAVE_BLUEZ
static dc_bluetooth_address_t
dc_address_get (const bdaddr_t *ba)
{
	dc_bluetooth_address_t address = 0;

	size_t shift = 0;
	for (size_t i = 0; i < C_ARRAY_SIZE(ba->b); ++i) {
		address |= (dc_bluetooth_address_t) ba->b[i] << shift;
		shift += 8;
	}

	return address;
}

static void
dc_address_set (bdaddr_t *ba, dc_bluetooth_address_t address)
{
	size_t shift = 0;
	for (size_t i = 0; i < C_ARRAY_SIZE(ba->b); ++i) {
		ba->b[i] = (address >> shift) & 0xFF;
		shift += 8;
	}
}
#endif
#endif

dc_status_t
dc_bluetooth_open (dc_iostream_t **out, dc_context_t *context)
{
#ifdef BLUETOOTH
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_bluetooth_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (dc_bluetooth_t *) dc_iostream_allocate (context, &dc_bluetooth_vtable);
	if (device == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

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
#ifdef _WIN32
	device->fd = socket (AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
#else
	device->fd = socket (AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
#endif
	if (device->fd == S_INVALID) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (context, errcode);
		status = syserror(errcode);
		goto error_wsacleanup;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_wsacleanup:
#ifdef _WIN32
	WSACleanup ();
error_free:
#endif
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef BLUETOOTH
static dc_status_t
dc_bluetooth_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;

	// Terminate all send and receive operations.
	shutdown (device->fd, 0);

	// Close the socket.
	if (S_CLOSE (device->fd) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror(errcode));
	}

#ifdef _WIN32
	// Terminate the winsock dll.
	if (WSACleanup () != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror(errcode));
	}
#endif

	return status;
}

static dc_status_t
dc_bluetooth_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;

	device->timeout = timeout;

	return DC_STATUS_SUCCESS;
}
#endif

dc_status_t
dc_bluetooth_discover (dc_iostream_t *abstract, dc_bluetooth_callback_t callback, void *userdata)
{
#ifdef BLUETOOTH
	dc_status_t status = DC_STATUS_SUCCESS;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

#ifdef _WIN32
	WSAQUERYSET wsaq;
	memset(&wsaq, 0, sizeof (wsaq));
	wsaq.dwSize = sizeof (wsaq);
	wsaq.dwNameSpace = NS_BTH;
	wsaq.lpcsaBuffer = NULL;

	HANDLE hLookup;
	if (WSALookupServiceBegin(&wsaq, LUP_CONTAINERS | LUP_FLUSHCACHE, &hLookup) != 0) {
		s_errcode_t errcode = S_ERRNO;
		if (errcode == WSASERVICE_NOT_FOUND) {
			// No remote bluetooth devices found.
			status = DC_STATUS_SUCCESS;
		} else {
			SYSERROR (abstract->context, errcode);
			status = syserror(errcode);
		}
		goto error_exit;
	}

	unsigned char buf[4096];
	LPWSAQUERYSET pwsaResults = (LPWSAQUERYSET) buf;
	memset(pwsaResults, 0, sizeof(WSAQUERYSET));
	pwsaResults->dwSize = sizeof(WSAQUERYSET);
	pwsaResults->dwNameSpace = NS_BTH;
	pwsaResults->lpBlob = NULL;

	while (1) {
		DWORD dwSize = sizeof(buf);
		if (WSALookupServiceNext (hLookup, LUP_RETURN_NAME | LUP_RETURN_ADDR, &dwSize, pwsaResults) != 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == WSA_E_NO_MORE || errcode == WSAENOMORE) {
				break; // No more results.
			}
			SYSERROR (abstract->context, errcode);
			status = syserror(errcode);
			goto error_close;
		}

		if (pwsaResults->dwNumberOfCsAddrs == 0 ||
			pwsaResults->lpcsaBuffer == NULL ||
			pwsaResults->lpcsaBuffer->RemoteAddr.lpSockaddr == NULL) {
			ERROR (abstract->context, "Invalid results returned");
			status = DC_STATUS_IO;
			goto error_close;
		}

		SOCKADDR_BTH *sa = (SOCKADDR_BTH *) pwsaResults->lpcsaBuffer->RemoteAddr.lpSockaddr;
		dc_bluetooth_address_t address = sa->btAddr;
		const char *name = (char *) pwsaResults->lpszServiceInstanceName;

		INFO (abstract->context, "Discover: address=" DC_ADDRESS_FORMAT ", name=%s", address, name);

		if (callback) callback (address, name, userdata);

	}

error_close:
	WSALookupServiceEnd (hLookup);
#else
	// Get the resource number for the first available bluetooth adapter.
	int dev = hci_get_route (NULL);
	if (dev < 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = syserror(errcode);
		goto error_exit;
	}

	// Open a socket to the bluetooth adapter.
	int fd = hci_open_dev (dev);
	if (fd < 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = syserror(errcode);
		goto error_exit;
	}

	// Allocate a buffer to store the results of the discovery.
	inquiry_info *devices = (inquiry_info *) malloc (MAX_DEVICES * sizeof(inquiry_info));
	if (devices == NULL) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = syserror(errcode);
		goto error_close;
	}

	// Perform the bluetooth device discovery. The inquiry lasts for at
	// most MAX_PERIODS * 1.28 seconds, and at most MAX_DEVICES devices
	// will be returned.
	int ndevices = hci_inquiry (dev, MAX_PERIODS, MAX_DEVICES, NULL, &devices, IREQ_CACHE_FLUSH);
	if (ndevices < 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = syserror(errcode);
		goto error_free;
	}

	for (unsigned int i = 0; i < ndevices; ++i) {
		dc_bluetooth_address_t address = dc_address_get (&devices[i].bdaddr);

		// Get the user friendly name.
		char buf[HCI_MAX_NAME_LENGTH], *name = buf;
		int rc = hci_read_remote_name (fd, &devices[i].bdaddr, sizeof(buf), buf, 0);
		if (rc < 0) {
			name = NULL;
		}

		INFO (abstract->context, "Discover: address=" DC_ADDRESS_FORMAT ", name=%s", address, name);

		if (callback) callback (address, name, userdata);
	}

error_free:
	free(devices);
error_close:
	hci_close_dev(fd);
#endif

error_exit:
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_bluetooth_connect (dc_iostream_t *abstract, dc_bluetooth_address_t address, unsigned int port)
{
#ifdef BLUETOOTH
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	INFO (abstract->context, "Connect: address=" DC_ADDRESS_FORMAT ", port=%d", address, port);

#ifdef _WIN32
	SOCKADDR_BTH sa;
	sa.addressFamily = AF_BTH;
	sa.btAddr = address;
	sa.port = port;
	memset(&sa.serviceClassId, 0, sizeof(sa.serviceClassId));
#else
	struct sockaddr_rc sa;
	sa.rc_family = AF_BLUETOOTH;
	sa.rc_channel = port;
	dc_address_set (&sa.rc_bdaddr, address);
#endif

	if (connect (device->fd, (struct sockaddr *) &sa, sizeof (sa)) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		return syserror(errcode);
	}

	return DC_STATUS_SUCCESS;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef BLUETOOTH
static dc_status_t
dc_bluetooth_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;

#ifdef _WIN32
	unsigned long bytes = 0;
#else
	int bytes = 0;
#endif

	if (S_IOCTL (device->fd, FIONREAD, &bytes) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		return syserror(errcode);
	}

	if (value)
		*value = bytes;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_bluetooth_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		struct timeval tvt;
		if (device->timeout > 0) {
			tvt.tv_sec  = (device->timeout / 1000);
			tvt.tv_usec = (device->timeout % 1000) * 1000;
		} else if (device->timeout == 0) {
			timerclear (&tvt);
		}

		int rc = select (device->fd + 1, &fds, NULL, NULL, device->timeout >= 0 ? &tvt : NULL);
		if (rc < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror(errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		s_ssize_t n = recv (device->fd, (char*) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR || errcode == S_EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
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
	if (actual)
		*actual = nbytes;

	return status;
}

static dc_status_t
dc_bluetooth_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_bluetooth_t *device = (dc_bluetooth_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (device->fd, &fds);

		int rc = select (device->fd + 1, NULL, &fds, NULL, NULL);
		if (rc < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror(errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		s_ssize_t n = send (device->fd, (const char *) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR || errcode == S_EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = syserror(errcode);
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
#endif
