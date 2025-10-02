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

#include "seac_screen_common.h"
#include "context-private.h"
#include "checksum.h"
#include "array.h"

int
seac_screen_record_isvalid (dc_context_t *context, const unsigned char data[], unsigned int size, unsigned int type, unsigned int id)
{
	// Check the record size.
	if (size != SZ_RECORD) {
		ERROR (context, "Unexpected record size (%u).", size);
		return 0;
	}

	// Check the record checksum.
	unsigned short csum = checksum_crc16_ccitt (data, size, 0xFFFF, 0x0000);
	if (csum != 0) {
		ERROR (context, "Unexpected record checksum (%04x).", csum);
		return 0;
	}

	// Check the record type.
	unsigned int rtype = data[size - 3];
	if (rtype != type) {
		ERROR (context, "Unexpected record type (%02x %02x).", rtype, type);
		return 0;
	}

	// Check the record id.
	unsigned int rid = array_uint32_le (data);
	if (rid != id) {
		ERROR (context, "Unexpected record id (%u %u).", rid, id);
		return 0;
	}

	return 1;
}
