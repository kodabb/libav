/*
 * Copyright (c) 2017 Vittorio Giovara <vittorio.giovara@gmail.com>
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

#include "mem.h"
#include "ambisonic.h"

AVAmbisonic *av_ambisonic_alloc(size_t *size, int nb_channels)
{
    AVAmbisonic *ambisonic;

    // maximum number usable in AVFrame->channel_layout
    if (nb_channels < 0 || nb_channels > 61)
        return NULL;

    ambisonic = av_mallocz(sizeof(AVAmbisonic));
    if (!ambisonic)
        return NULL;

    ambisonic->nb_channels = nb_channels;
    ambisonic->channel_map = av_malloc_array(nb_channels, sizeof(unsigned));
    if (nb_channels && !ambisonic->channel_map) {
        av_free(ambisonic);
        return NULL;
    }

    if (size)
        *size = sizeof(*ambisonic);

    return ambisonic;
}

void av_ambisonic_free(AVAmbisonic **ambisonic)
{
    AVAmbisonic *internal = *ambisonic;
    av_free(internal->channel_map);
    av_freep(ambisonic);
}
