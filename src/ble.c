/*
 * libdivecomputer
 *
 * Copyright (C) 2024 Jef Driesen
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

#include <string.h>

#include <libdivecomputer/ble.h>

#include "platform.h"

char *
dc_ble_uuid2str (const dc_ble_uuid_t uuid, char *str, size_t size)
{
	if (str == NULL || size < DC_BLE_UUID_SIZE)
		return NULL;

	int n = dc_platform_snprintf(str, size,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		uuid[0], uuid[1], uuid[2], uuid[3],
		uuid[4], uuid[5],
		uuid[6], uuid[7],
		uuid[8], uuid[9],
		uuid[10], uuid[11], uuid[12],
		uuid[13], uuid[14], uuid[15]);
	if (n < 0 || (size_t) n >= size)
		return NULL;

	return str;
}

int
dc_ble_str2uuid (const char *str, dc_ble_uuid_t uuid)
{
	dc_ble_uuid_t tmp = {0};

	if (str == NULL || uuid == NULL)
		return 0;

	unsigned int i = 0;
	unsigned char c = 0;
	while ((c = *str++) != '\0') {
		if (c == '-') {
			if (i != 8 && i != 12 && i != 16 && i != 20) {
				return 0; /* Invalid character! */
			}
			continue;
		} else if (c >= '0' && c <= '9') {
			c -= '0';
		} else if (c >= 'A' && c <= 'F') {
			c -= 'A' - 10;
		} else if (c >= 'a' && c <= 'f') {
			c -= 'a' - 10;
		} else {
			return 0; /* Invalid character! */
		}

		if ((i & 1) == 0) {
			c <<= 4;
		}

		if (i >= 2 * sizeof(tmp)) {
			return 0; /* Too many characters! */
		}

		tmp[i / 2] |= c;
		i++;
	}

	if (i != 2 * sizeof(tmp)) {
		return 0; /* Not enough characters! */
	}

	memcpy (uuid, tmp, sizeof(tmp));

	return 1;
}
