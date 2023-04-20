/*
 * libdivecomputer
 *
 * Copyright (C) 2011 Jef Driesen
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

#ifndef DC_COMMON_H
#define DC_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dc_status_t {
	DC_STATUS_SUCCESS = 0,
	DC_STATUS_DONE = 1,
	DC_STATUS_UNSUPPORTED = -1,
	DC_STATUS_INVALIDARGS = -2,
	DC_STATUS_NOMEMORY = -3,
	DC_STATUS_NODEVICE = -4,
	DC_STATUS_NOACCESS = -5,
	DC_STATUS_IO = -6,
	DC_STATUS_TIMEOUT = -7,
	DC_STATUS_PROTOCOL = -8,
	DC_STATUS_DATAFORMAT = -9,
	DC_STATUS_CANCELLED = -10
} dc_status_t;

typedef enum dc_transport_t {
	DC_TRANSPORT_NONE      = 0,
	DC_TRANSPORT_SERIAL    = (1 << 0),
	DC_TRANSPORT_USB       = (1 << 1),
	DC_TRANSPORT_USBHID    = (1 << 2),
	DC_TRANSPORT_IRDA      = (1 << 3),
	DC_TRANSPORT_BLUETOOTH = (1 << 4),
	DC_TRANSPORT_BLE       = (1 << 5)
} dc_transport_t;

typedef enum dc_family_t {
	DC_FAMILY_NULL = 0,
	/* Suunto */
	DC_FAMILY_SUUNTO_SOLUTION = (1 << 16),
	DC_FAMILY_SUUNTO_EON,
	DC_FAMILY_SUUNTO_VYPER,
	DC_FAMILY_SUUNTO_VYPER2,
	DC_FAMILY_SUUNTO_D9,
	DC_FAMILY_SUUNTO_EONSTEEL,
	/* Reefnet */
	DC_FAMILY_REEFNET_SENSUS = (2 << 16),
	DC_FAMILY_REEFNET_SENSUSPRO,
	DC_FAMILY_REEFNET_SENSUSULTRA,
	/* Uwatec */
	DC_FAMILY_UWATEC_ALADIN = (3 << 16),
	DC_FAMILY_UWATEC_MEMOMOUSE,
	DC_FAMILY_UWATEC_SMART,
	DC_FAMILY_UWATEC_MERIDIAN, /* Deprecated: integrated into the Uwatec Smart family. */
	DC_FAMILY_UWATEC_G2, /* Deprecated: integrated into the Uwatec Smart family. */
	/* Oceanic */
	DC_FAMILY_OCEANIC_VTPRO = (4 << 16),
	DC_FAMILY_OCEANIC_VEO250,
	DC_FAMILY_OCEANIC_ATOM2,
	DC_FAMILY_PELAGIC_I330R,
	/* Mares */
	DC_FAMILY_MARES_NEMO = (5 << 16),
	DC_FAMILY_MARES_PUCK,
	DC_FAMILY_MARES_DARWIN,
	DC_FAMILY_MARES_ICONHD,
	/* Heinrichs Weikamp */
	DC_FAMILY_HW_OSTC = (6 << 16),
	DC_FAMILY_HW_FROG,
	DC_FAMILY_HW_OSTC3,
	/* Cressi */
	DC_FAMILY_CRESSI_EDY = (7 << 16),
	DC_FAMILY_CRESSI_LEONARDO,
	DC_FAMILY_CRESSI_GOA,
	/* Zeagle */
	DC_FAMILY_ZEAGLE_N2ITION3 = (8 << 16),
	/* Atomic Aquatics */
	DC_FAMILY_ATOMICS_COBALT = (9 << 16),
	/* Shearwater */
	DC_FAMILY_SHEARWATER_PREDATOR = (10 << 16),
	DC_FAMILY_SHEARWATER_PETREL,
	/* Dive Rite */
	DC_FAMILY_DIVERITE_NITEKQ = (11 << 16),
	/* Citizen */
	DC_FAMILY_CITIZEN_AQUALAND = (12 << 16),
	/* DiveSystem */
	DC_FAMILY_DIVESYSTEM_IDIVE = (13 << 16),
	/* Cochran */
	DC_FAMILY_COCHRAN_COMMANDER = (14 << 16),
	/* Tecdiving */
	DC_FAMILY_TECDIVING_DIVECOMPUTEREU = (15 << 16),
	/* McLean */
	DC_FAMILY_MCLEAN_EXTREME = (16 << 16),
	/* Liquivision */
	DC_FAMILY_LIQUIVISION_LYNX = (17 << 16),
	/* Sporasub */
	DC_FAMILY_SPORASUB_SP2 = (18 << 16),
	/* Deep Six */
	DC_FAMILY_DEEPSIX_EXCURSION = (19 << 16),
	/* Seac Screen */
	DC_FAMILY_SEAC_SCREEN = (20 << 16),
	/* Deepblu Cosmiq */
	DC_FAMILY_DEEPBLU_COSMIQ = (21 << 16),
	/* Oceans S1 */
	DC_FAMILY_OCEANS_S1 = (22 << 16),
	/* Divesoft Freedom */
	DC_FAMILY_DIVESOFT_FREEDOM = (23 << 16),
	/* Halcyon Symbios */
	DC_FAMILY_HALCYON_SYMBIOS = (24 << 16),
} dc_family_t;

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DC_COMMON_H */
