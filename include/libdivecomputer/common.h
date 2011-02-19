/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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

#ifndef DC_COMMON_H
#define DC_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dc_status_t {
	DC_STATUS_SUCCESS = 0,
	DC_STATUS_UNSUPPORTED = -1,
	DC_STATUS_INVALIDARGS = -2,
	DC_STATUS_NOMEMORY = -3,
	DC_STATUS_NODEVICE = -4,
	DC_STATUS_NOACCESS = -5,
	DC_STATUS_IO = -6,
	DC_STATUS_TIMEOUT = -7,
	DC_STATUS_PROTOCOL = -8,
	DC_STATUS_DATAFORMAT = -9,
	DC_STATUS_CANCELLED = -10
} dc_status_t;

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_COMMON_H */
