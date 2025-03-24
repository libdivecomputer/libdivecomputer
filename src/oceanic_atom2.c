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

#include <string.h> // memcpy
#include <stdlib.h> // malloc, free

#include <libdivecomputer/ble.h>

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_atom2_device_vtable.base)

#define MAXPACKET  256
#define MAXRETRIES 2
#define MAXDELAY   16
#define INVALID    0xFFFFFFFF

#define CMD_INIT      0xA8
#define CMD_VERSION   0x84
#define CMD_HANDSHAKE 0xE5
#define CMD_READ1     0xB1
#define CMD_READ8     0xB4
#define CMD_READ16    0xB8
#define CMD_READ16HI  0xF6
#define CMD_WRITE     0xB2
#define CMD_KEEPALIVE 0x91
#define CMD_QUIT      0x6A

#define ACK 0x5A
#define NAK 0xA5

#define REPEAT 50

typedef struct oceanic_atom2_device_t {
	oceanic_common_device_t base;
	dc_iostream_t *iostream;
	unsigned int handshake_repeat;
	unsigned int handshake_counter;
	unsigned int sequence;
	unsigned int delay;
	unsigned int extra;
	unsigned int bigpage;
	unsigned char cache[256];
	unsigned int cached_page;
	unsigned int cached_highmem;
} oceanic_atom2_device_t;

static dc_status_t oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_close (dc_device_t *abstract);

static const oceanic_common_device_vtable_t oceanic_atom2_device_vtable = {
	{
		sizeof(oceanic_atom2_device_t),
		DC_FAMILY_OCEANIC_ATOM2,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		oceanic_atom2_device_read, /* read */
		oceanic_atom2_device_write, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		NULL, /* timesync */
		oceanic_atom2_device_close /* close */
	},
	oceanic_common_device_devinfo,
	oceanic_common_device_pointers,
	oceanic_common_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_layout_t aeris_f10_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0D80, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	3, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_f11_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0D80, /* rb_profile_begin */
	0x20000, /* rb_profile_end */
	0, /* pt_mode_global */
	2, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_default_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom1_layout = {
	0x8000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0440, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0440, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2a_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2b_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2c_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t sherwood_wisdom_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03D0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_proplus3_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03E0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t tusa_zenair_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_oc1_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_oci_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x1400, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom3_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_vt4_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0420, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t hollis_tx1_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0780, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x1000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_veo1_layout = {
	0x0400, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0400, /* rb_profile_begin */
	0x0400, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_reactpro_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0600, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x0600, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	1, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_proplusx_layout = {
	0x440000, /* memsize */
	0x40000, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x1000, /* rb_logbook_begin */
	0x10000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x40000, /* rb_profile_begin */
	0x440000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aqualung_i770r_layout = {
	0x640000, /* memsize */
	0x40000, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x2000, /* rb_logbook_begin */
	0x10000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x40000, /* rb_profile_begin */
	0x640000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_a300cs_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0900, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x1000, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aqualung_i450t_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	1, /* rb_logbook_direction */
	0x1400, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_version_t versions[] = {
	{"OCEVEO10 \0\0   8K", 0,      VEO10,         &oceanic_veo1_layout},
	{"AERIS XR1 NX R\0\0", 0,      XR1NX,         &oceanic_veo1_layout},

	{"ATOM rev\0\0  256K", 0,      ATOM1,         &oceanic_atom1_layout},

	{"MANTA  R\0\0  512K", 0x3242, MANTA,         &oceanic_atom2a_layout},
	{"MANTA  R\0\0  512K", 0,      MANTA,         &oceanic_atom2c_layout},
	{"2M ATOM r\0\0 512K", 0x3349, ATOM2,         &oceanic_atom2a_layout},
	{"2M ATOM r\0\0 512K", 0,      ATOM2,         &oceanic_atom2c_layout},
	{"OCE GEO R\0\0 512K", 0x3242, GEO,           &oceanic_atom2a_layout},
	{"OCE GEO R\0\0 512K", 0,      GEO,           &oceanic_atom2c_layout},

	{"INSIGHT2 \0\0 512K", 0,      INSIGHT2,      &oceanic_atom2a_layout},
	{"OCEVEO30 \0\0 512K", 0,      VEO30,         &oceanic_atom2a_layout},
	{"ATMOSAI R\0\0 512K", 0,      ATMOSAI2,      &oceanic_atom2a_layout},
	{"PROPLUS2 \0\0 512K", 0,      PROPLUS21,     &oceanic_atom2a_layout},
	{"OCEGEO20 \0\0 512K", 0,      GEO20,         &oceanic_atom2a_layout},
	{"AQUAI200 \0\0 512K", 0,      I200,          &oceanic_atom2a_layout},
	{"AQUA200C \0\0 512K", 0,      I200C,         &oceanic_atom2a_layout},

	{"ELEMENT2 \0\0 512K", 0,      ELEMENT2,      &oceanic_atom2b_layout},
	{"OCEVEO20 \0\0 512K", 0,      VEO20,         &oceanic_atom2b_layout},
	{"TUSAZEN \0\0  512K", 0,      ZEN,           &oceanic_atom2b_layout},
	{"AQUAI300 \0\0 512K", 0,      I300,          &oceanic_atom2b_layout},
	{"HOLLDG03 \0\0 512K", 0,      DG03,          &oceanic_atom2b_layout},
	{"AQUAI100 \0\0 512K", 0,      I100,          &oceanic_atom2b_layout},
	{"AQUA300C \0\0 512K", 0,      I300C,         &oceanic_atom2b_layout},
	{"OCEGEO40 \0\0 512K", 0,      GEO40,         &oceanic_atom2b_layout},
	{"VEOSMART \0\0 512K", 0,      VEO40,         &oceanic_atom2b_layout},

	{"2M EPIC r\0\0 512K", 0,      EPICA,         &oceanic_atom2c_layout},
	{"EPIC1  R\0\0  512K", 0,      EPICB,         &oceanic_atom2c_layout},
	{"AERIA300 \0\0 512K", 0,      A300,          &oceanic_atom2c_layout},

	{"OCE VT3 R\0\0 512K", 0,      VT3,           &oceanic_default_layout},
	{"ELITET3 R\0\0 512K", 0,      T3A,           &oceanic_default_layout},
	{"ELITET31 \0\0 512K", 0,      T3B,           &oceanic_default_layout},
	{"DATAMASK \0\0 512K", 0,      DATAMASK,      &oceanic_default_layout},
	{"COMPMASK \0\0 512K", 0,      COMPUMASK,     &oceanic_default_layout},

	{"WISDOM R\0\0  512K", 0x3342, WISDOM3,       &sherwood_wisdom_layout},
	{"WISDOM R\0\0  512K", 0,      WISDOM2,       &sherwood_wisdom_layout},

	{"PROPLUS3 \0\0 512K", 0,      PROPLUS3,      &oceanic_proplus3_layout},
	{"PROPLUS4 \0\0 512K", 0,      PROPLUS4,      &oceanic_proplus3_layout},

	{"TUZENAIR \0\0 512K", 0,      ZENAIR,        &tusa_zenair_layout},
	{"AMPHOSSW \0\0 512K", 0,      AMPHOS,        &tusa_zenair_layout},
	{"AMPHOAIR \0\0 512K", 0,      AMPHOSAIR,     &tusa_zenair_layout},
	{"VOYAGE2G \0\0 512K", 0,      VOYAGER2G,     &tusa_zenair_layout},
	{"TUSTALIS \0\0 512K", 0,      TALIS,         &tusa_zenair_layout},
	{"AMPHOS20 \0\0 512K", 0,      AMPHOS2,       &tusa_zenair_layout},
	{"AMPAIR20 \0\0 512K", 0,      AMPHOSAIR2,    &tusa_zenair_layout},

	{"REACPRO2 \0\0 512K", 0,      REACTPROWHITE, &oceanic_reactpro_layout},

	{"FREEWAER \0\0 512K", 0,      F10A,          &aeris_f10_layout},
	{"OCEANF10 \0\0 512K", 0,      F10B,          &aeris_f10_layout},
	{"MUNDIAL R\0\0 512K", 0x3300, MUNDIAL3,      &aeris_f10_layout},
	{"MUNDIAL R\0\0 512K", 0,      MUNDIAL2,      &aeris_f10_layout},

	{"AERISF11 \0\0 1024", 0,      F11A,          &aeris_f11_layout},
	{"OCEANF11 \0\0 1024", 0,      F11B,          &aeris_f11_layout},

	{"OCWATCH R\0\0 1024", 0,      OC1A,          &oceanic_oc1_layout},
	{"OC1WATCH \0\0 1024", 0,      OC1B,          &oceanic_oc1_layout},
	{"OCSWATCH \0\0 1024", 0,      OCS,           &oceanic_oc1_layout},
	{"AQUAI550 \0\0 1024", 0,      I550,          &oceanic_oc1_layout},
	{"AQUA550C \0\0 1024", 0,      I550C,         &oceanic_oc1_layout},
	{"WISDOM04 \0\0 1024", 0,      WISDOM4,       &oceanic_oc1_layout},
	{"AQUA470C \0\0 1024", 0,      I470TC,        &oceanic_oc1_layout},
	{"AQUA200C \0\0 1024", 0,      I200CV2,       &oceanic_oc1_layout},
	{"GEOAIR   \0\0 1024", 0,      GEOAIR,        &oceanic_oc1_layout},

	{"OCEANOCI \0\0 1024", 0,      OCI,           &oceanic_oci_layout},

	{"OCEATOM3 \0\0 1024", 0,      ATOM3,         &oceanic_atom3_layout},
	{"ATOM31  \0\0  1024", 0,      ATOM31,        &oceanic_atom3_layout},

	{"OCEANVT4 \0\0 1024", 0,      VT4,           &oceanic_vt4_layout},
	{"OCEAVT41 \0\0 1024", 0,      VT41,          &oceanic_vt4_layout},
	{"AERISAIR \0\0 1024", 0,      A300AI,        &oceanic_vt4_layout},
	{"SWVISION \0\0 1024", 0,      VISION,        &oceanic_vt4_layout},
	{"XPSUBAIR \0\0 1024", 0,      XPAIR,         &oceanic_vt4_layout},
	{"AQUAI100 \0\0 1024", 0,      I100V2,        &oceanic_vt4_layout},

	{"HOLLDG04 \0\0 2048", 0,      TX1,           &hollis_tx1_layout},

	{"AER300CS \0\0 2048", 0,      A300CS,        &aeris_a300cs_layout},
	{"OCEANVTX \0\0 2048", 0,      VTX,           &aeris_a300cs_layout},
	{"AQUAI750 \0\0 2048", 0,      I750TC,        &aeris_a300cs_layout},
	{"SWDRAGON \0\0 2048", 0,      SAGE,          &aeris_a300cs_layout},
	{"SWBEACON \0\0 2048", 0,      BEACON,        &aeris_a300cs_layout},

	{"AQUAI450 \0\0 2048", 0,      I450T,         &aqualung_i450t_layout},

	{"OCEANOCX \0\0 \0\0\0\0", 0,  PROPLUSX,      &oceanic_proplusx_layout},

	{"AQUA770R \0\0 \0\0\0\0", 0,  I770R,         &aqualung_i770r_layout},
};

/*
 * The BLE GATT packet size is up to 20 bytes and the format is:
 *
 * byte 0: <0xCD>
 *         Seems to always have this value. Don't ask what it means
 * byte 1: <d 1 c s s s s s>
 *          d=0 means "command", d=1 means "reply from dive computer"
 *          1 is always set, afaik
 *          c=0 means "last packet" in sequence, c=1 means "more packets coming"
 *          sssss is a 5-bit sequence number for packets
 * byte 2: <cmd seq>
 *          starts at 0 for the connection, incremented for each command
 * byte 3: <length of data>
 *          1-16 bytes of data per packet.
 * byte 4..n: <data>
 */
static dc_status_t
oceanic_atom2_ble_write (oceanic_atom2_device_t *device, const unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	unsigned char buf[20];
	unsigned char cmd_seq = device->sequence;
	unsigned char pkt_seq = 0;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		unsigned char status = 0x40;
		unsigned int length = size - nbytes;
		if (length > sizeof(buf) - 4) {
			length = sizeof(buf) - 4;
			status |= 0x20;
		}
		buf[0] = 0xcd;
		buf[1] = status | (pkt_seq & 0x1F);
		buf[2] = cmd_seq;
		buf[3] = length;
		memcpy (buf + 4, data, length);

		rc = dc_iostream_write (device->iostream, buf, 4 + length, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += length;
		pkt_seq++;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceanic_atom2_ble_read (oceanic_atom2_device_t *device, unsigned char data[], unsigned int size, unsigned int *actual)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char buf[20];
	unsigned char cmd_seq = device->sequence;
	unsigned char pkt_seq = 0;

	unsigned int nbytes = 0;
	while (1) {
		size_t transferred = 0;
		rc = dc_iostream_read (device->iostream, buf, sizeof(buf), &transferred);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		if (transferred < 4) {
			ERROR (abstract->context, "Invalid packet size (" DC_PRINTF_SIZE ").", transferred);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the start byte.
		if (buf[0] != 0xcd) {
			ERROR (abstract->context, "Unexpected packet start byte (%02x).", buf[0]);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the status byte.
		unsigned char status = buf[1];
		unsigned char expect = 0xc0 | (pkt_seq & 0x1F) | (status & 0x20);
		if (status != expect) {
			ERROR (abstract->context, "Unexpected packet status byte (%02x %02x).", status, expect);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the sequence byte.
		if (buf[2] != cmd_seq) {
			ERROR (abstract->context, "Unexpected packet sequence byte (%02x %02x).", buf[2], cmd_seq);
			return DC_STATUS_PROTOCOL;
		}

		// Verify the length byte.
		unsigned int length = buf[3];
		if (length + 4 > transferred) {
			ERROR (abstract->context, "Invalid packet length (%u).", length);
			return DC_STATUS_PROTOCOL;
		}

		// Append the payload data to the output buffer. If the output
		// buffer is too small, the error is not reported immediately
		// but delayed until all packets have been received.
		if (nbytes < size) {
			unsigned int n = length;
			if (nbytes + n > size) {
				n = size - nbytes;
			}
			memcpy (data + nbytes, buf + 4, n);
		}
		nbytes += length;
		pkt_seq++;

		// Last packet?
		if ((status & 0x20) == 0)
			break;
	}

	// Verify the expected number of bytes.
	if (nbytes > size) {
		ERROR (abstract->context, "Unexpected number of bytes received (%u %u).", nbytes, size);
		return DC_STATUS_PROTOCOL;
	}

	if (actual) {
		*actual = nbytes;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceanic_atom2_packet (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char ack, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);

	if (asize > MAXPACKET) {
		return DC_STATUS_INVALIDARGS;
	}

	if (crc_size > 2 || (crc_size != 0 && asize == 0)) {
		return DC_STATUS_INVALIDARGS;
	}

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (device->delay) {
		dc_iostream_sleep (device->iostream, device->delay);
	}

	// Send the command to the dive computer.
	if (transport == DC_TRANSPORT_BLE) {
		status = oceanic_atom2_ble_write (device, command, csize);
	} else {
		status = dc_iostream_write (device->iostream, command, csize, NULL);
	}
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer of the dive computer.
	unsigned char packet[1 + MAXPACKET + 2];
	unsigned int nbytes = 1 + asize + crc_size;
	if (transport == DC_TRANSPORT_BLE) {
		// Accept excess bytes for some models.
		if (asize && device->extra) {
			nbytes = 1 + MAXPACKET + crc_size;
		}
		status = oceanic_atom2_ble_read (device, packet, nbytes, &nbytes);
	} else {
		status = dc_iostream_read (device->iostream, packet, 1 + asize + crc_size, NULL);
	}
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the number of bytes.
	if (nbytes < 1) {
		ERROR (abstract->context, "Invalid packet size (%u).", nbytes);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the ACK byte of the answer.
	if (packet[0] != ack) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		if (packet[0] == (unsigned char) ~ack) {
			return DC_STATUS_UNSUPPORTED;
		} else {
			return DC_STATUS_PROTOCOL;
		}
	}

	// Verify the number of bytes.
	if (nbytes < 1 + asize + crc_size) {
		ERROR (abstract->context, "Unexpected number of bytes received (%u %u).", nbytes, 1 + asize + crc_size);
		return DC_STATUS_PROTOCOL;
	}

	nbytes -= 1 + crc_size;

	if (asize) {
		// Verify the checksum of the answer.
		unsigned short crc, ccrc;
		if (crc_size == 2) {
			crc = array_uint16_le (packet + 1 + nbytes);
			ccrc = checksum_add_uint16 (packet + 1, nbytes, 0x0000);
		} else {
			crc = packet[1 + nbytes];
			ccrc = checksum_add_uint8 (packet + 1, nbytes, 0x00);
		}
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}

		memcpy (answer, packet + 1, asize);
	}

	if (nbytes > asize) {
		WARNING (abstract->context, "Ignored %u excess byte(s).", nbytes - asize);
	}

	device->sequence++;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char ack, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_atom2_packet (device, command, csize, ack, answer, asize, crc_size)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Increase the inter packet delay.
		if (device->delay < MAXDELAY)
			device->delay++;

		// Delay the next attempt.
		dc_iostream_sleep (device->iostream, 100);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

/*
 * The BLE communication sends a handshake packet that seems
 * to be a passphrase based on the BLE name of the device
 * (more specifically the serial number encoded in the name).
 *
 * The packet format is:
 *    0xe5
 *    < 8 bytes of passphrase >
 *    one-byte checksum of the passphrase.
 */
static dc_status_t
oceanic_atom2_ble_handshake(oceanic_atom2_device_t *device)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Retrieve the bluetooth device name.
	// The format of the name is something like 'FQ001124', where the
	// two first letters are the ASCII representation of the model
	// number (e.g. 'FQ' or 0x4651 for the i770R), and the six digits
	// are the serial number.
	char name[8 + 1] = {0};
	rc = dc_iostream_ioctl (device->iostream, DC_IOCTL_BLE_GET_NAME, name, sizeof(name));
	if (rc != DC_STATUS_SUCCESS) {
		if (rc == DC_STATUS_UNSUPPORTED) {
			// Allow skipping the handshake if no name. But the download
			// will likely fail.
			WARNING (abstract->context, "Bluetooth device name unavailable.");
			return DC_STATUS_SUCCESS;
		} else {
			return rc;
		}
	}

	// Force a null terminated string.
	name[sizeof(name) - 1] = 0;

	// Check the minimum length.
	if (strlen (name) < 8) {
		ERROR (abstract->context, "Bluetooth device name too short.");
		return DC_STATUS_IO;
	}

	// Turn ASCII numbers into just raw byte values.
	unsigned char handshake[10] = {CMD_HANDSHAKE};
	for (unsigned int i = 0; i < 6; i++) {
		handshake[i + 1] = name[i + 2] - '0';
	}

	// Add simple checksum.
	handshake[9] = checksum_add_uint8 (handshake + 1, 8, 0x00);

	// Send the command to the dive computer.
	rc = oceanic_atom2_transfer (device, handshake, sizeof(handshake), ACK, NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS) {
		if (rc == DC_STATUS_UNSUPPORTED) {
			WARNING (abstract->context, "Bluetooth handshake not supported.");
			return DC_STATUS_SUCCESS;
		} else {
			return rc;
		}
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
oceanic_atom2_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceanic_atom2_device_t *) dc_device_allocate (context, &oceanic_atom2_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Set the default values.
	device->iostream = iostream;
	device->delay = 0;
	device->extra = model == PROPLUSX || model == I770R;
	device->sequence = 0;
	device->bigpage = 1; // no big pages
	device->cached_page = INVALID;
	device->cached_highmem = INVALID;
	memset(device->cache, 0, sizeof(device->cache));

	// Get the correct baudrate.
	unsigned int baudrate = 38400;
	if (model == VTX || model == I750TC || model == PROPLUSX || model == I770R) {
		baudrate = 115200;
	}

	// Set the serial communication protocol (38400 8N1).
	status = dc_iostream_configure (device->iostream, baudrate, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Set the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	// Clear the RTS line to reset the PIC inside the data cable as it
	// may not have have been previously cleared. This ensures that the
	// PIC will always start in a known state once RTS is set. Starting
	// in a known default state is very important as the PIC won't
	// respond to init commands unless it is in a default state.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Hold RTS clear for a bit to allow PIC to reset.
	dc_iostream_sleep (device->iostream, 100);

	// Set the RTS line.
	status = dc_iostream_set_rts (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_free;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_iostream_sleep (device->iostream, 100);

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (automatically activated
	// by connecting the device), or already in download mode.
	status = oceanic_atom2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	HEXDUMP (context, DC_LOGLEVEL_DEBUG, "Version", device->base.version, sizeof (device->base.version));

	if (dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE) {
		status = oceanic_atom2_ble_handshake(device);
		if (status != DC_STATUS_SUCCESS) {
			goto error_free;
		}
	}

	// Detect the memory layout.
	const oceanic_common_version_t *version = OCEANIC_COMMON_MATCH(device->base.version, versions, &device->base.firmware);
	if (version == NULL) {
		WARNING (context, "Unsupported device detected!");
		if (memcmp(device->base.version + 12, "256K", 4) == 0) {
			device->base.layout = &oceanic_atom1_layout;
		} else if (memcmp(device->base.version + 12, "512K", 4) == 0) {
			device->base.layout = &oceanic_default_layout;
		} else if (memcmp(device->base.version + 12, "1024", 4) == 0) {
			device->base.layout = &oceanic_oc1_layout;
		} else if (memcmp(device->base.version + 12, "2048", 4) == 0) {
			device->base.layout = &hollis_tx1_layout;
		} else {
			device->base.layout = &oceanic_default_layout;
		}
		device->base.model = 0;
	} else {
		device->base.layout = version->layout;
		device->base.model = version->model;
	}

	// Set the big page support.
	if (device->base.layout == &aeris_f11_layout ||
		device->base.layout == &oceanic_proplus3_layout) {
		device->bigpage = 8;
	} else if (device->base.layout == &oceanic_proplusx_layout ||
		device->base.layout == &aqualung_i770r_layout ||
		device->base.layout == &aeris_a300cs_layout) {
		device->bigpage = 16;
	}

	// Repeat the handshaking every few packets.
	device->handshake_repeat = dc_iostream_get_transport (device->iostream) == DC_TRANSPORT_BLE &&
		device->base.model == PROPLUS4;
	device->handshake_counter = 0;

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
oceanic_atom2_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the quit command.
	unsigned char command[4] = {CMD_QUIT, 0x05, 0xA5};
	rc = oceanic_atom2_transfer (device, command, sizeof (command), NAK, NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
oceanic_atom2_device_keepalive (dc_device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char command[] = {CMD_KEEPALIVE, 0x05, 0xA5};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), ACK, NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_atom2_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	unsigned char command[] = {CMD_VERSION};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), ACK, data, PAGESIZE, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;
	const oceanic_common_layout_t *layout = device->base.layout;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	// Pick the correct read command and number of checksum bytes.
	unsigned char read_cmd = 0x00;
	unsigned int crc_size = 0;
	switch (device->bigpage) {
	case 1:
		read_cmd = CMD_READ1;
		crc_size = 1;
		break;
	case 8:
		read_cmd = CMD_READ8;
		crc_size = device->base.model == PROPLUS4 ? 2 : 1;
		break;
	case 16:
		read_cmd = CMD_READ16;
		crc_size = 2;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Pick the best pagesize to use.
	unsigned int pagesize = device->bigpage * PAGESIZE;

	// High memory state.
	unsigned int highmem = 0;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Switch to the correct read command when entering the high memory area.
		if (layout->highmem && address >= layout->highmem && !highmem) {
			highmem = layout->highmem;
			read_cmd = CMD_READ16HI;
			crc_size = 2;
			pagesize = 16 * PAGESIZE;
		}

		// Calculate the page number after mapping the virtual high memory
		// addresses back to their physical address.
		unsigned int page = (address - highmem) / pagesize;

		if (page != device->cached_page || highmem != device->cached_highmem) {
			if (device->handshake_repeat && ++device->handshake_counter % REPEAT == 0) {
				unsigned char version[PAGESIZE] = {0};
				oceanic_atom2_device_version (abstract, version, sizeof (version));
				oceanic_atom2_ble_handshake (device);
			}

			// Read the package.
			unsigned int number = highmem ? page : page * device->bigpage; // This is always PAGESIZE, even in big page mode.
			unsigned char command[] = {read_cmd,
					(number >> 8) & 0xFF, // high
					(number     ) & 0xFF, // low
				};
			dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), ACK, device->cache, pagesize, crc_size);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Cache the page.
			device->cached_page = page;
			device->cached_highmem = highmem;
		}

		unsigned int offset = address % pagesize;
		unsigned int length = pagesize - offset;
		if (nbytes + length > size)
			length = size - nbytes;

		memcpy (data, device->cache + offset, length);

		nbytes += length;
		address += length;
		data += length;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	// Invalidate the cache.
	device->cached_page = INVALID;
	device->cached_highmem = INVALID;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Prepare to write the package.
		unsigned int number = address / PAGESIZE;
		unsigned char prepare[] = {CMD_WRITE,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
			};
		dc_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), ACK, NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char command[PAGESIZE + 1] = {0};
		memcpy (command, data, PAGESIZE);
		command[PAGESIZE] = checksum_add_uint8 (command, PAGESIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), ACK, NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DC_STATUS_SUCCESS;
}
