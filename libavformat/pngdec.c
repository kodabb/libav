/*
 * RAW PNG video demuxer
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

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rawdec.h"

static int png_probe(AVProbeData *p)
{
    if (AV_RB64(p->buf) == 0x89504e470d0a1a0a)
        return AVPROBE_SCORE_MAX;

    return 0;
}

FF_DEF_RAWVIDEO_DEMUXER(png, "png image", png_probe, "png", AV_CODEC_ID_PNG)
