/*
 * Copyright (c) 2013 Vittorio Giovara <vittorio.giovara@gmail.com>
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
#include "common.h"
#include "stereo3d.h"

AVStereo3D *av_stereo3d_alloc(void)
{
    AVStereo3D *data = av_mallocz(sizeof(*data));

    if (!data)
        return NULL;

    return data;
}

void av_stereo3d_free(AVStereo3D **data)
{
    if (!data || !*data)
        return;

    av_freep(data);
}

const char *av_stereo3d_name(const enum AVStereo3DType type)
{
    switch (type) {
    default:
        return "Unknown";
    case AV_STEREO3D_2D:
        return "2D";
    case AV_STEREO3D_FRAMESEQUENCE:
        return "Frame sequence";
    case AV_STEREO3D_CHECKERS:
        return "Checkerboard";
    case AV_STEREO3D_LINES:
        return "Line-interleaved";
    case AV_STEREO3D_COLUMNS:
        return "Column-interleaved";
    case AV_STEREO3D_SIDEBYSIDE:
        return "Side by side";
    case AV_STEREO3D_TOPBOTTOM:
        return "Top and bottom";
    case AV_STEREO3D_TILES:
        return "Tile";
    case AV_STEREO3D_ANAGLYPH:
        return "Anaglyph";
    case AV_STEREO3D_MULTISTREAM:
        return "Multistream";
    }
}
