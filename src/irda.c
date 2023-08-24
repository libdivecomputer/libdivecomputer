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

#include <libdivecomputer/irda.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_iostream_isinstance((device), &dc_irda_vtable)

#define DISCOVER_MAX_DEVICES 16	// Maximum number of devices.
#define DISCOVER_MAX_RETRIES 4	// Maximum number of retries.

#ifdef _WIN32
#define DISCOVER_BUFSIZE sizeof (DEVICELIST) + \
				sizeof (IRDA_DEVICE_INFO) * (DISCOVER_MAX_DEVICES - 1)
#else
#define DISCOVER_BUFSIZE sizeof (struct irda_device_list) + \
				sizeof (struct irda_device_info) * (DISCOVER_MAX_DEVICES - 1)
#endif

struct dc_irda_device_t {
	unsigned int address;
	unsigned int charset;
	unsigned int hints;
	char name[22];
};

#ifdef IRDA
static dc_status_t dc_irda_iterator_next (dc_iterator_t *iterator, void *item);

typedef struct dc_irda_iterator_t {
	dc_iterator_t base;
	dc_irda_device_t items[DISCOVER_MAX_DEVICES];
	size_t count;
	size_t current;
} dc_irda_iterator_t;

static const dc_iterator_vtable_t dc_irda_iterator_vtable = {
	sizeof(dc_irda_iterator_t),
	dc_irda_iterator_next,
	NULL,
};

static const dc_iostream_vtable_t dc_irda_vtable = {
	sizeof(dc_socket_t),
	dc_socket_set_timeout, /* set_timeout */
	NULL, /* set_break */
	NULL, /* set_dtr */
	NULL, /* set_rts */
	NULL, /* get_lines */
	dc_socket_get_available, /* get_available */
	NULL, /* configure */
	dc_socket_poll, /* poll */
	dc_socket_read, /* read */
	dc_socket_write, /* write */
	dc_socket_ioctl, /* ioctl */
	NULL, /* flush */
	NULL, /* purge */
	dc_socket_sleep, /* sleep */
	dc_socket_close, /* close */
};
#endif

unsigned int
dc_irda_device_get_address (dc_irda_device_t *device)
{
	if (device == NULL)
		return 0;

	return device->address;
}

const char *
dc_irda_device_get_name (dc_irda_device_t *device)
{
	if (device == NULL || device->name[0] == '\0')
		return NULL;

	return device->name;
}

void
dc_irda_device_free (dc_irda_device_t *device)
{
	free (device);
}

dc_status_t
dc_irda_iterator_new (dc_iterator_t **out, dc_context_t *context, dc_descriptor_t *descriptor)
{
#ifdef IRDA
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_irda_iterator_t *iterator = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_irda_iterator_t *) dc_iterator_allocate (context, &dc_irda_iterator_vtable);
	if (iterator == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the socket library.
	status = dc_socket_init (context);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	// Open the socket.
	s_socket_t fd = socket (AF_IRDA, SOCK_STREAM, 0);
	if (fd == S_INVALID) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (context, errcode);
		status = dc_socket_syserror(errcode);
		goto error_socket_exit;
	}

	unsigned char data[DISCOVER_BUFSIZE] = {0};
#ifdef _WIN32
	DEVICELIST *list = (DEVICELIST *) data;
#else
	struct irda_device_list *list = (struct irda_device_list *) data;
#endif
	s_socklen_t size = sizeof (data);

	int rc = 0;
	unsigned int nretries = 0;
	while ((rc = getsockopt (fd, SOL_IRLMP, IRLMP_ENUMDEVICES, (char *) data, &size)) != 0 ||
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
				SYSERROR (context, errcode);
				status = dc_socket_syserror(errcode);
				goto error_socket_close;
			}
		}

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= DISCOVER_MAX_RETRIES) {
			break;
		}

		// Restore the size parameter in case it was
		// modified by the previous getsockopt call.
		size = sizeof (data);

		dc_platform_sleep (1000);
	}

	S_CLOSE (fd);
	dc_socket_exit (context);

	unsigned int count = 0;
#ifdef _WIN32
	for (size_t i = 0; i < list->numDevice; ++i) {
		const char *name = list->Device[i].irdaDeviceName;
		unsigned int address = array_uint32_le (list->Device[i].irdaDeviceID);
		unsigned int charset = list->Device[i].irdaCharSet;
		unsigned int hints = (list->Device[i].irdaDeviceHints1 << 8) +
								list->Device[i].irdaDeviceHints2;
#else
	for (size_t i = 0; i < list->len; ++i) {
		const char *name = list->dev[i].info;
		unsigned int address = list->dev[i].daddr;
		unsigned int charset = list->dev[i].charset;
		unsigned int hints = array_uint16_be (list->dev[i].hints);
#endif

		INFO (context, "Discover: address=%08x, name=%s, charset=%02x, hints=%04x",
			address, name, charset, hints);

		if (!dc_descriptor_filter (descriptor, DC_TRANSPORT_IRDA, name)) {
			continue;
		}

		strncpy(iterator->items[count].name, name, sizeof(iterator->items[count].name) - 1);
		iterator->items[count].name[sizeof(iterator->items[count].name) - 1] = '\0';
		iterator->items[count].address = address;
		iterator->items[count].charset = charset;
		iterator->items[count].hints = hints;
		count++;
	}

	iterator->current = 0;
	iterator->count = count;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;

error_socket_close:
	S_CLOSE (fd);
error_socket_exit:
	dc_socket_exit (context);
error_free:
	dc_iterator_deallocate ((dc_iterator_t *) iterator);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}

#ifdef IRDA
static dc_status_t
dc_irda_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_irda_iterator_t *iterator = (dc_irda_iterator_t *) abstract;
	dc_irda_device_t *device = NULL;

	if (iterator->current >= iterator->count)
		return DC_STATUS_DONE;

	device = (dc_irda_device_t *) malloc (sizeof(dc_irda_device_t));
	if (device == NULL) {
		SYSERROR (abstract->context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	*device = iterator->items[iterator->current++];

	*(dc_irda_device_t **) out = device;

	return DC_STATUS_SUCCESS;
}
#endif

dc_status_t
dc_irda_open (dc_iostream_t **out, dc_context_t *context, unsigned int address, unsigned int lsap)
{
#ifdef IRDA
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: address=%08x, lsap=%u", address, lsap);

	// Allocate memory.
	device = (dc_socket_t *) dc_iostream_allocate (context, &dc_irda_vtable, DC_TRANSPORT_IRDA);
	if (device == NULL) {
		SYSERROR (context, S_ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	// Open the socket.
	status = dc_socket_open (&device->base, AF_IRDA, SOCK_STREAM, 0);
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

#ifdef _WIN32
	SOCKADDR_IRDA peer;
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address      ) & 0xFF;
	peer.irdaDeviceID[1] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[2] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[3] = (address >> 24) & 0xFF;
	dc_platform_snprintf (peer.irdaServiceName, sizeof(peer.irdaServiceName), "LSAP-SEL%u", lsap);
#else
	struct sockaddr_irda peer;
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	peer.sir_lsap_sel = lsap;
	memset (peer.sir_name, 0x00, sizeof(peer.sir_name));
#endif

	status = dc_socket_connect (&device->base, (struct sockaddr *) &peer, sizeof (peer));
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_socket_close (&device->base);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
#else
	return DC_STATUS_UNSUPPORTED;
#endif
}
