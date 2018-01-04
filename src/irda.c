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
#include <stdio.h>	// snprintf
#include <string.h>

#include "socket.h"

#ifdef _WIN32
#ifdef HAVE_AF_IRDA_H
#define IRDA
#include <af_irda.h>
#endif
#else
#ifdef HAVE_LINUX_IRDA_H
#define IRDA
#include <linux/types.h>
#include <linux/irda.h>
#endif
#endif

#include "irda.h"

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_irda_vtable)

#ifdef IRDA
static const dc_iostream_vtable_t dc_irda_vtable = {
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
#endif

dc_status_t
dc_irda_open (dc_iostream_t **out, dc_context_t *context)
{
#ifdef IRDA
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (dc_socket_t *) dc_iostream_allocate (context, &dc_irda_vtable);
	if (device == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Open the socket.
	status = dc_socket_open (&device->base, AF_IRDA, SOCK_STREAM, 0);
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
dc_irda_discover (dc_iostream_t *abstract, dc_irda_callback_t callback, void *userdata)
{
#ifdef IRDA
	dc_socket_t *device = (dc_socket_t *) abstract;

	if (!ISINSTANCE (abstract))
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
				SYSERROR (abstract->context, errcode);
				return dc_socket_syserror(errcode);
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

			INFO (abstract->context,
				"Discover: address=%08x, name=%s, charset=%02x, hints=%04x",
				address, name, charset, hints);

			callback (address, name, charset, hints, userdata);
		}
	}

	return DC_STATUS_SUCCESS;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_irda_connect_name (dc_iostream_t *abstract, unsigned int address, const char *name)
{
#ifdef IRDA
	dc_socket_t *device = (dc_socket_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	INFO (abstract->context, "Connect: address=%08x, name=%s", address, name ? name : "");

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address      ) & 0xFF;
	peer.irdaDeviceID[1] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[2] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[3] = (address >> 24) & 0xFF;
	if (name) {
		strncpy (peer.irdaServiceName, name, sizeof(peer.irdaServiceName) - 1);
		peer.irdaServiceName[sizeof(peer.irdaServiceName) - 1] = '\0';
	} else {
		memset (peer.irdaServiceName, 0x00, sizeof(peer.irdaServiceName));
	}
#else
	struct sockaddr_irda peer;
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	if (name) {
		strncpy (peer.sir_name, name, sizeof(peer.sir_name) - 1);
		peer.sir_name[sizeof(peer.sir_name) - 1] = '\0';
	} else {
		memset (peer.sir_name, 0x00, sizeof(peer.sir_name));
	}
#endif

	return dc_socket_connect (&device->base, (struct sockaddr *) &peer, sizeof (peer));
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

dc_status_t
dc_irda_connect_lsap (dc_iostream_t *abstract, unsigned int address, unsigned int lsap)
{
#ifdef IRDA
	dc_socket_t *device = (dc_socket_t *) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	INFO (abstract->context, "Connect: address=%08x, lsap=%u", address, lsap);

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

	return dc_socket_connect (&device->base, (struct sockaddr *) &peer, sizeof (peer));
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}
