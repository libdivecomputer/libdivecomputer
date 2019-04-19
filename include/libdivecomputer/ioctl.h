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

#ifndef DC_IOCTL_H
#define DC_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 * Ioctl direction bits.
 *
 * Note: WRITE means the application is writing and the driver is
 * reading. READ means the application is reading and the driver is
 * writing.
 */
#define DC_IOCTL_DIR_NONE  0u
#define DC_IOCTL_DIR_READ  1u
#define DC_IOCTL_DIR_WRITE 2u

/*
 * Ioctl variable size bits.
 */
#define DC_IOCTL_SIZE_VARIABLE 0

/*
 * Helper macro to encode ioctl numbers.
 */
#define DC_IOCTL_BASE(dir,type,nr,size) \
	(((dir)  << 30) | \
	 ((size) << 16) | \
	 ((type) <<  8) | \
	 ((nr)   <<  0))

/*
 * Macros to encode ioctl numbers.
 */
#define DC_IOCTL_IO(type,nr)        DC_IOCTL_BASE(DC_IOCTL_DIR_NONE, (type), (nr), 0)
#define DC_IOCTL_IOR(type,nr,size)  DC_IOCTL_BASE(DC_IOCTL_DIR_READ, (type), (nr), (size))
#define DC_IOCTL_IOW(type,nr,size)  DC_IOCTL_BASE(DC_IOCTL_DIR_WRITE, (type), (nr), (size))
#define DC_IOCTL_IORW(type,nr,size) DC_IOCTL_BASE(DC_IOCTL_DIR_READ | DC_IOCTL_DIR_WRITE, (type), (nr), (size))

/*
 * Macros to decode ioctl numbers.
 */
#define DC_IOCTL_DIR(request)  (((request) >> 30) & 0x0003)
#define DC_IOCTL_SIZE(request) (((request) >> 16) & 0x3FFF)
#define DC_IOCTL_TYPE(request) (((request) >>  8) & 0x00FF)
#define DC_IOCTL_NR(request)   (((request) >>  0) & 0x00FF)

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_IOCTL_H */
