/*
 * libdivecomputer
 *
 * Copyright (C) 2019 Jef Driesen
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

#ifndef DC_BLE_H
#define DC_BLE_H

#include "ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Get the remote device name.
 */
#define DC_IOCTL_BLE_GET_NAME   DC_IOCTL_IOR('b', 0, DC_IOCTL_SIZE_VARIABLE)

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_BLE_H */
