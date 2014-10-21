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

#ifndef UNITS_H
#define UNITS_H


#define POUND       0.45359237
#define FEET        0.3048
#define INCH        0.0254
#define GRAVITY     9.80665
#define ATM         101325.0
#define BAR         100000.0
#define FSW         (ATM / 33.0)
#define MSW         (BAR / 10.0)
#define PSI         ((POUND * GRAVITY) / (INCH * INCH))
#define CUFT        (FEET * FEET * FEET)


#endif /* UNITS_H */
