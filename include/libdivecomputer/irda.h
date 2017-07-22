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

#ifndef DC_IRDA_H
#define DC_IRDA_H

#include "common.h"
#include "context.h"
#include "iostream.h"
#include "iterator.h"
#include "descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque object representing an IrDA device.
 */
typedef struct dc_irda_device_t dc_irda_device_t;

/**
 * Get the address of the IrDA device.
 *
 * @param[in]  device  A valid IrDA device.
 */
unsigned int
dc_irda_device_get_address (dc_irda_device_t *device);

/**
 * Get the name of the IrDA device.
 *
 * @param[in]  device  A valid IrDA device.
 */
const char *
dc_irda_device_get_name (dc_irda_device_t *device);

/**
 * Destroy the IrDA device and free all resources.
 *
 * @param[in]  device  A valid IrDA device.
 */
void
dc_irda_device_free (dc_irda_device_t *device);

/**
 * Create an iterator to enumerate the IrDA devices.
 *
 * @param[out] iterator    A location to store the iterator.
 * @param[in]  context     A valid context object.
 * @param[in]  descriptor  A valid device descriptor or NULL.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_iterator_new (dc_iterator_t **iterator, dc_context_t *context, dc_descriptor_t *descriptor);

/**
 * Open an IrDA connection.
 *
 * @param[out]  iostream A location to store the IrDA connection.
 * @param[in]   context  A valid context object.
 * @param[in]   address  The IrDA device address.
 * @param[in]   lsap     The IrDA LSAP number.
 * @returns #DC_STATUS_SUCCESS on success, or another #dc_status_t code
 * on failure.
 */
dc_status_t
dc_irda_open (dc_iostream_t **iostream, dc_context_t *context, unsigned int address, unsigned int lsap);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IRDA_H */
