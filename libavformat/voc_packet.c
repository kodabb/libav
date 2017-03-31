/*
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

#include "avformat.h"
#include "internal.h"
#include "voc.h"

int
ff_voc_get_packet(AVFormatContext *s, AVPacket *pkt, AVStream *st, int max_size)
{
    VocDecContext *voc = s->priv_data;
    AVCodecParameters *par = st->codecpar;
    AVIOContext *pb = s->pb;
    VocType type;
    int size, tmp_codec=-1;
    int sample_rate = 0;
    int channels = 1;

    while (!voc->remaining_size) {
        type = avio_r8(pb);
        if (type == VOC_TYPE_EOF)
            return AVERROR(EIO);
        voc->remaining_size = avio_rl24(pb);
        if (!voc->remaining_size) {
            if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL))
                return AVERROR(EIO);
            voc->remaining_size = avio_size(pb) - avio_tell(pb);
        }
        max_size -= 4;

        switch (type) {
        case VOC_TYPE_VOICE_DATA:
            if (!par->sample_rate) {
                par->sample_rate = 1000000 / (256 - avio_r8(pb));
                if (sample_rate)
                    par->sample_rate = sample_rate;
                avpriv_set_pts_info(st, 64, 1, par->sample_rate);
                par->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
                par->ch_layout.nb_channels = channels;
                par->bits_per_coded_sample = av_get_bits_per_sample(par->codec_id);
            } else
                avio_skip(pb, 1);
            tmp_codec = avio_r8(pb);
            voc->remaining_size -= 2;
            max_size -= 2;
            channels = 1;
            break;

        case VOC_TYPE_VOICE_DATA_CONT:
            break;

        case VOC_TYPE_EXTENDED:
            sample_rate = avio_rl16(pb);
            avio_r8(pb);
            channels = avio_r8(pb) + 1;
            sample_rate = 256000000 / (channels * (65536 - sample_rate));
            voc->remaining_size = 0;
            max_size -= 4;
            break;

        case VOC_TYPE_NEW_VOICE_DATA:
            if (!par->sample_rate) {
                par->sample_rate = avio_rl32(pb);
                avpriv_set_pts_info(st, 64, 1, par->sample_rate);
                par->bits_per_coded_sample = avio_r8(pb);
                channels = avio_r8(pb);
                par->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
                par->ch_layout.nb_channels = channels;
            } else
                avio_skip(pb, 6);
            tmp_codec = avio_rl16(pb);
            avio_skip(pb, 4);
            voc->remaining_size -= 12;
            max_size -= 12;
            break;

        default:
            avio_skip(pb, voc->remaining_size);
            max_size -= voc->remaining_size;
            voc->remaining_size = 0;
            break;
        }
    }

    if (tmp_codec >= 0) {
        tmp_codec = ff_codec_get_id(ff_voc_codec_tags, tmp_codec);
        if (par->codec_id == AV_CODEC_ID_NONE)
            par->codec_id = tmp_codec;
        else if (par->codec_id != tmp_codec)
            av_log(s, AV_LOG_WARNING, "Ignoring mid-stream change in audio codec\n");
        if (par->codec_id == AV_CODEC_ID_NONE) {
            if (s->audio_codec_id == AV_CODEC_ID_NONE) {
                av_log(s, AV_LOG_ERROR, "unknown codec tag\n");
                return AVERROR(EINVAL);
            }
            av_log(s, AV_LOG_WARNING, "unknown codec tag\n");
        }
    }

    par->bit_rate = par->sample_rate * par->bits_per_coded_sample;

    if (max_size <= 0)
        max_size = 2048;
    size = FFMIN(voc->remaining_size, max_size);
    voc->remaining_size -= size;
    return av_get_packet(pb, pkt, size);
}
