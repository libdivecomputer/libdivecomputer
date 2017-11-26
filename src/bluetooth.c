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

#include "socket.h"

#ifdef _WIN32
#ifdef HAVE_WS2BTH_H
#define BLUETOOTH
#include <ws2bth.h>
#endif
#else
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
#define DC_ADDRESS_FORMAT "%012I64X"
#else
#define DC_ADDRESS_FORMAT "%012llX"
#endif

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

#define MAX_DEVICES 255
#define MAX_PERIODS 8

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_bluetooth_vtable)

#ifdef BLUETOOTH
static const dc_iostream_vtable_t dc_bluetooth_vtable = {
	sizeof(dc_socket_t),
	dc_socket_set_timeout, /* set_timeout */
	dc_socket_set_latency, /* set_latency */
	dc_socket_set_halfduplex, /* set_halfduplex */
	dc_socket_set_break, /* set_break */
	dc_socket_set_dtr, /* set_dtr */
	dc_socket_set_rts, /* set_rts */
	dc_socket_get_lines, /* get_lines */
	dc_socket_get_available, /* get_received */
	dc_socket_configure, /* configure */
	dc_socket_read, /* read */
	dc_socket_write, /* write */
	dc_socket_flush, /* flush */
	dc_socket_purge, /* purge */
	dc_socket_sleep, /* sleep */
	dc_socket_close, /* close */
};

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
	dc_socket_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (dc_socket_t *) dc_iostream_allocate (context, &dc_bluetooth_vtable);
	if (device == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Open the socket.
#ifdef _WIN32
	status = dc_socket_open (&device->base, AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
#else
	status = dc_socket_open (&device->base, AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
#endif
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

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
			status = dc_socket_syserror(errcode);
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
			status = dc_socket_syserror(errcode);
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
		status = dc_socket_syserror(errcode);
		goto error_exit;
	}

	// Open a socket to the bluetooth adapter.
	int fd = hci_open_dev (dev);
	if (fd < 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = dc_socket_syserror(errcode);
		goto error_exit;
	}

	// Allocate a buffer to store the results of the discovery.
	inquiry_info *devices = (inquiry_info *) malloc (MAX_DEVICES * sizeof(inquiry_info));
	if (devices == NULL) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = dc_socket_syserror(errcode);
		goto error_close;
	}

	// Perform the bluetooth device discovery. The inquiry lasts for at
	// most MAX_PERIODS * 1.28 seconds, and at most MAX_DEVICES devices
	// will be returned.
	int ndevices = hci_inquiry (dev, MAX_PERIODS, MAX_DEVICES, NULL, &devices, IREQ_CACHE_FLUSH);
	if (ndevices < 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = dc_socket_syserror(errcode);
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
	dc_socket_t *device = (dc_socket_t *) abstract;

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

	return dc_socket_connect (&device->base, (struct sockaddr *) &sa, sizeof (sa));
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}
