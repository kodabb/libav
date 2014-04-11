/*
 * Copyright (c) 2014 Vittorio Giovara <vittorio.giovara@gmail.com>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_DISPLAY_H
#define AVUTIL_DISPLAY_H

#include <stdint.h>

#include "libavutil/rational.h"

/**
 * @return the rotation angle in degrees.
 */
int av_display_rotation_angle(const uint8_t *matrix);

/**
 * Convert a 3x3 matrix into an array to be stored in a side data.
 *
 * @return a newly allocated array.
 *
 * @note   callers should free the returned memory with av_freep().
 */
uint8_t *av_display_matrix_to_data(const int32_t matrix[3][3]);

/**
 * Convert an angle expressed in degrees to a 3x3 matrix in fixed point
 * numbers (16.16 for columns 1 and 2 and 2.30 for column 3).
 * The array can be immediately stored in a side data; in order to
 * access the elements it is necessary to read them as little endian.
 *
 * @param  angle rotation expressed in degrees.
 * @return a newly allocated array.
 *
 * @note   callers should free the returned memory with av_freep().
 */
uint8_t *av_display_angle_to_matrix(const double angle);

#endif /* AVUTIL_DISPLAY_H */
