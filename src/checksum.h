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

#ifndef CHECKSUM_H
#define CHECKSUM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

unsigned char
checksum_add_uint4 (const unsigned char data[], unsigned int size, unsigned char init);

unsigned char
checksum_add_uint8 (const unsigned char data[], unsigned int size, unsigned char init);

unsigned short
checksum_add_uint16 (const unsigned char data[], unsigned int size, unsigned short init);

unsigned char
checksum_xor_uint8 (const unsigned char data[], unsigned int size, unsigned char init);

unsigned char
checksum_crc8 (const unsigned char data[], unsigned int size, unsigned char init, unsigned char xorout);

unsigned short
checksum_crc16_ccitt (const unsigned char data[], unsigned int size, unsigned short init, unsigned short xorout);

unsigned short
checksum_crc16r_ccitt (const unsigned char data[], unsigned int size, unsigned short init, unsigned short xorout);

unsigned short
checksum_crc16_ansi (const unsigned char data[], unsigned int size, unsigned short init, unsigned short xorout);

unsigned short
checksum_crc16r_ansi (const unsigned char data[], unsigned int size, unsigned short init, unsigned short xorout);

unsigned int
checksum_crc32r (const unsigned char data[], unsigned int size);

unsigned int
checksum_crc32 (const unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CHECKSUM_H */
