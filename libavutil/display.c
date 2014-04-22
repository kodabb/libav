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

#include <stdint.h>
#include <string.h>
#include <math.h>

#include "display.h"
#include "intreadwrite.h"
#include "mem.h"

// fixed point to double
#define CONV_FP(x) ((double) (x)) / (1 << 16)

// double to fixed point
#define CONV_DB(x) (int32_t) ((x) * (1 << 16))

uint8_t *av_display_matrix_to_data(const int32_t matrix[3][3])
{
    int i, j;
    uint8_t *buf = av_mallocz(sizeof(int32_t) * 3 * 3);
    uint8_t *src = buf;

    if (!buf)
        return NULL;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            AV_WL32(src, matrix[j][i]);
            src += 4;
        }
    }

    return buf;
}

int av_display_rotation_angle(const uint8_t *matrix)
{
    double rotationf, scale[2];
    int32_t display_matrix[3][3];
    const uint8_t *buf;
    int i, j;

    buf = matrix;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            display_matrix[j][i] = AV_RL32(buf);
            buf += 4;
        }
    }

    scale[0] = sqrt(CONV_FP(display_matrix[0][0]) * CONV_FP(display_matrix[0][0]) +
                    CONV_FP(display_matrix[1][0]) * CONV_FP(display_matrix[1][0]));
    scale[1] = sqrt(CONV_FP(display_matrix[0][1]) * CONV_FP(display_matrix[0][1]) +
                    CONV_FP(display_matrix[1][1]) * CONV_FP(display_matrix[1][1]));

    rotationf = atan2(CONV_FP(display_matrix[0][1]) / scale[1],
                      CONV_FP(display_matrix[0][0]) / scale[0]) * 180 / M_PI;

    return (int) floor(rotationf);
}

uint8_t *av_display_angle_to_matrix(double angle)
{
    int32_t display_matrix[3][3];
    double radians = angle * M_PI / 180.0f;

    memset(display_matrix, 0, sizeof(int32_t) * 3 * 3);

    display_matrix[0][0] = CONV_DB(cos(radians));
    display_matrix[0][1] = CONV_DB(-sin(radians));
    display_matrix[1][0] = CONV_DB(sin(radians));
    display_matrix[1][1] = CONV_DB(cos(radians));
    display_matrix[2][2] = 1 << 30;

    return av_display_matrix_to_data(display_matrix);
}

uint8_t *av_display_translate_matrix(const uint8_t *matrix,
                                     unsigned int x, unsigned int y)
{
    int32_t display_matrix[3][3];
    const uint8_t *buf;
    int i, j;

    buf = matrix;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            display_matrix[j][i] = AV_RL32(buf);
            buf += 4;
        }
    }

    display_matrix[2][0] = x << 16;
    display_matrix[2][1] = y << 16;

    buf = matrix;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            AV_WL32(buf, display_matrix[j][i]);
            buf += 4;
        }
    }

    return matrix;
}

uint8_t *av_display_flip_matrix(const uint8_t *matrix, int hflip, int vflip)
{
    int32_t display_matrix[3][3];
    int32_t flip[3][3];
    int32_t result[3][3];
    const uint8_t *buf;
    int i, j, k;

    if (!hflip && !vflip)
        return matrix;

    memset(flip, 0, sizeof(int32_t) * 3 * 3);
    flip[0][0] = 1 - 2 * (!!hflip);
    flip[1][1] = 1 - 2 * (!!vflip);
    flip[2][2] = 1;

    buf = matrix;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            display_matrix[j][i] = AV_RL32(buf);
            buf += 4;
        }
    }

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            result[i][j] = 0;
            for (k = 0; k < 3; k++)
                result[i][j] += display_matrix[i][k] * flip[k][j];
        }
    }

    buf = matrix;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            AV_WL32(buf, result[j][i]);
            buf += 4;
        }
    }

    return matrix;
}
