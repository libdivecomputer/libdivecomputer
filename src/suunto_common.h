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

#ifndef SUUNTO_COMMON_H
#define SUUNTO_COMMON_H

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef int (*fp_compare_t) (device_t *device, const unsigned char data[], unsigned int size);

device_status_t
suunto_common_extract_dives (device_t *device, const unsigned char data[], unsigned int begin, unsigned int end, unsigned int eop, unsigned int peek, fp_compare_t fp_compare, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_COMMON_H */
