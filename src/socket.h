/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#ifndef DC_SOCKET_H
#define DC_SOCKET_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>      // errno
#include <unistd.h>     // close
#include <sys/types.h>  // socket, getsockopt
#include <sys/socket.h> // socket, getsockopt
#include <sys/select.h> // select
#include <sys/ioctl.h>  // ioctl
#include <sys/time.h>
#include <time.h>
#endif

#include <libdivecomputer/common.h>
#include <libdivecomputer/context.h>

#include "iostream-private.h"

#ifdef _WIN32
typedef SOCKET s_socket_t;
typedef int s_ssize_t;
typedef DWORD s_errcode_t;
typedef int s_socklen_t;
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
typedef int s_socket_t;
typedef ssize_t s_ssize_t;
typedef int s_errcode_t;
typedef socklen_t s_socklen_t;
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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dc_socket_t {
	dc_iostream_t base;
	s_socket_t fd;
	int timeout;
} dc_socket_t;

dc_status_t
dc_socket_syserror (s_errcode_t errcode);

dc_status_t
dc_socket_init (dc_context_t *context);

dc_status_t
dc_socket_exit (dc_context_t *context);

dc_status_t
dc_socket_open (dc_iostream_t *iostream, int family, int type, int protocol);

dc_status_t
dc_socket_close (dc_iostream_t *iostream);

dc_status_t
dc_socket_connect (dc_iostream_t *iostream, const struct sockaddr *addr, s_socklen_t addrlen);

dc_status_t
dc_socket_set_timeout (dc_iostream_t *iostream, int timeout);

dc_status_t
dc_socket_get_available (dc_iostream_t *iostream, size_t *value);

dc_status_t
dc_socket_poll (dc_iostream_t *iostream, int timeout);

dc_status_t
dc_socket_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);

dc_status_t
dc_socket_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);

dc_status_t
dc_socket_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);

dc_status_t
dc_socket_sleep (dc_iostream_t *abstract, unsigned int timeout);

dc_status_t
dc_socket_close (dc_iostream_t *iostream);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_SOCKET_H */
