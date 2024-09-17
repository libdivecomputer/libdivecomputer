/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#ifndef OCEANIC_COMMON_H
#define OCEANIC_COMMON_H

#include "device-private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

// vtpro
#define AERIS500AI    0x4151
#define VERSAPRO      0x4155
#define ATMOS2        0x4158
#define PROPLUS2      0x4159
#define ATMOSAI       0x4244
#define VTPRO         0x4245
#define WISDOM        0x4246
#define ELITE         0x424F

// veo250
#define REACTPRO      0x4247
#define VEO200        0x424B
#define VEO250        0x424C
#define XP5           0x4251
#define VEO180        0x4252
#define XR2           0x4255
#define INSIGHT       0x425A
#define DG02          0x4352

// atom2
#define ATOM1         0x4250
#define EPICA         0x4257
#define VT3           0x4258
#define T3A           0x4259
#define ATOM2         0x4342
#define GEO           0x4344
#define MANTA         0x4345
#define XR1NX         0x4346
#define DATAMASK      0x4347
#define COMPUMASK     0x4348
#define F10A          0x434D
#define OC1A          0x434E
#define WISDOM2       0x4350
#define INSIGHT2      0x4353
#define REACTPROWHITE 0x4354
#define ELEMENT2      0x4357
#define VEO10         0x4358
#define VEO20         0x4359
#define VEO30         0x435A
#define ZEN           0x4441
#define ZENAIR        0x4442
#define ATMOSAI2      0x4443
#define PROPLUS21     0x4444
#define GEO20         0x4446
#define VT4           0x4447
#define OC1B          0x4449
#define VOYAGER2G     0x444B
#define ATOM3         0x444C
#define DG03          0x444D
#define OCS           0x4450
#define OC1C          0x4451
#define VT41          0x4452
#define EPICB         0x4453
#define T3B           0x4455
#define ATOM31        0x4456
#define A300AI        0x4457
#define WISDOM3       0x4458
#define A300          0x445A
#define TX1           0x4542
#define MUNDIAL2      0x4543
#define AMPHOS        0x4545
#define AMPHOSAIR     0x4546
#define PROPLUS3      0x4548
#define F11A          0x4549
#define OCI           0x454B
#define A300CS        0x454C
#define TALIS         0x454E
#define MUNDIAL3      0x4550
#define PROPLUSX      0x4552
#define F10B          0x4553
#define F11B          0x4554
#define XPAIR         0x4555
#define VISION        0x4556
#define VTX           0x4557
#define I300          0x4559
#define I750TC        0x455A
#define I450T         0x4641
#define I550          0x4642
#define I200          0x4646
#define SAGE          0x4647
#define I300C         0x4648
#define I200C         0x4649
#define I100          0x464E
#define I770R         0x4651
#define I550C         0x4652
#define GEO40         0x4653
#define VEO40         0x4654
#define WISDOM4       0x4655
#define PROPLUS4      0x4656
#define AMPHOS2       0x4657
#define AMPHOSAIR2    0x4658
#define BEACON        0x4742
#define I470TC        0x4743
#define I100V2        0x4745
#define I200CV2       0x4749
#define GEOAIR        0x474B

// i330r
#define DSX           0x4741
#define I330R         0x4744
#define I330R_C       0x474D

#define PAGESIZE 0x10
#define FPMAXSIZE 0x200

#define OCEANIC_COMMON_MATCH(version,patterns,firmware) \
	oceanic_common_match ((version), (patterns), \
	sizeof (patterns) / sizeof *(patterns), (firmware))

typedef struct oceanic_common_layout_t {
	// Memory size.
	unsigned int memsize;
	unsigned int highmem;
	// Device info.
	unsigned int cf_devinfo;
	// Ringbuffer pointers.
	unsigned int cf_pointers;
	// Logbook ringbuffer.
	unsigned int rb_logbook_begin;
	unsigned int rb_logbook_end;
	unsigned int rb_logbook_entry_size;
	unsigned int rb_logbook_direction;
	// Profile ringbuffer
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
	// The pointer mode indicates how the global ringbuffer pointers
	// should be interpreted (a first/last or a begin/end pair), and
	// how the profile pointers are stored in each logbook entry (two
	// 12-bit values or two 16-bit values with each 4 bits padding).
	unsigned int pt_mode_global;
	unsigned int pt_mode_logbook;
	unsigned int pt_mode_serial;
} oceanic_common_layout_t;

typedef struct oceanic_common_device_t {
	dc_device_t base;
	unsigned int firmware;
	unsigned char version[PAGESIZE];
	unsigned char fingerprint[FPMAXSIZE];
	unsigned int model;
	const oceanic_common_layout_t *layout;
	unsigned int multipage;
} oceanic_common_device_t;

typedef struct oceanic_common_device_vtable_t {
	dc_device_vtable_t base;
	dc_status_t (*devinfo) (dc_device_t *device, dc_event_progress_t *progress);
	dc_status_t (*pointers) (dc_device_t *device, dc_event_progress_t *progress, unsigned int *rb_logbook_begin, unsigned int *rb_logbook_end, unsigned int *rb_profile_begin, unsigned int *rb_profile_end);
	dc_status_t (*logbook) (dc_device_t *device, dc_event_progress_t *progress, dc_buffer_t *logbook, unsigned int begin, unsigned int end);
	dc_status_t (*profile) (dc_device_t *device, dc_event_progress_t *progress, dc_buffer_t *logbook, dc_dive_callback_t callback, void *userdata);
} oceanic_common_device_vtable_t;

typedef struct oceanic_common_version_t {
	unsigned char pattern[PAGESIZE + 1];
	unsigned int firmware;
	unsigned int model;
	const oceanic_common_layout_t *layout;
} oceanic_common_version_t;

const oceanic_common_version_t *
oceanic_common_match (const unsigned char *version, const oceanic_common_version_t patterns[], size_t n, unsigned int *firmware);

void
oceanic_common_device_init (oceanic_common_device_t *device);

dc_status_t
oceanic_common_device_devinfo (dc_device_t *device, dc_event_progress_t *progress);

dc_status_t
oceanic_common_device_pointers (dc_device_t *device, dc_event_progress_t *progress,
	unsigned int *rb_logbook_begin, unsigned int *rb_logbook_end,
	unsigned int *rb_profile_begin, unsigned int *rb_profile_end);

dc_status_t
oceanic_common_device_logbook (dc_device_t *device, dc_event_progress_t *progress, dc_buffer_t *logbook, unsigned int begin, unsigned int end);

dc_status_t
oceanic_common_device_profile (dc_device_t *device, dc_event_progress_t *progress, dc_buffer_t *logbook, dc_dive_callback_t callback, void *userdata);

dc_status_t
oceanic_common_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);

dc_status_t
oceanic_common_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);

dc_status_t
oceanic_common_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* OCEANIC_COMMON_H */
