/*
 * libdivecomputer
 *
 * Copyright (C) 2025 Jef Driesen
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

#ifndef SEAC_SCREEN_COMMON_H
#define SEAC_SCREEN_COMMON_H

#include <libdivecomputer/context.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define HEADER1 0xCF
#define HEADER2 0xC0
#define SAMPLE  0xAA

#define SZ_RECORD  64
#define SZ_HEADER  (SZ_RECORD * 2)
#define SZ_SAMPLE  SZ_RECORD

int
seac_screen_record_isvalid (dc_context_t *context, const unsigned char data[], unsigned int size, unsigned int type, unsigned int id);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SEAC_SCREEN_COMMON_H */
