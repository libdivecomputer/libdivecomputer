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

#ifndef IRDA_H
#define IRDA_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct irda_t irda_t;

typedef void (*irda_callback_t) (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata);

int irda_errcode (void);

const char* irda_errmsg (void);

int irda_init (void);

int irda_cleanup (void);

int irda_socket_open (irda_t **device);

int irda_socket_close (irda_t *device);

int irda_socket_set_timeout (irda_t *device, long timeout);

int irda_socket_discover (irda_t *device, irda_callback_t callback, void *userdata);

int irda_socket_connect_name (irda_t *device, unsigned int address, const char *name);
int irda_socket_connect_lsap (irda_t *device, unsigned int address, unsigned int lsap);

int irda_socket_available (irda_t *device);

int irda_socket_read (irda_t *device, void *data, unsigned int size);

int irda_socket_write (irda_t *device, const void *data, unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* IRDA_H */
