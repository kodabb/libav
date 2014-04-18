/*
 * ADTS muxer.
 * Copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@smartjog.com>
 *                    Mans Rullgard <mans@mansr.com>
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

#include "libavutil/bitstream.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/mpeg4audio.h"
#include "avformat.h"

#define ADTS_HEADER_SIZE 7

typedef struct {
    int write_adts;
    int objecttype;
    int sample_rate_index;
    int channel_conf;
    int pce_size;
    uint8_t pce_data[MAX_PCE_SIZE];
} ADTSContext;

#define ADTS_MAX_FRAME_BYTES ((1 << 13) - 1)

static int adts_decode_extradata(AVFormatContext *s, ADTSContext *adts, uint8_t *buf, int size)
{
    AVGetBitContext gb;
    AVPutBitContext pb;
    MPEG4AudioConfig m4ac;
    int off;

    av_bitstream_get_init(&gb, buf, size * 8);
    off = avpriv_mpeg4audio_get_config(&m4ac, buf, size * 8, 1);
    if (off < 0)
        return off;
    av_bitstream_skip_long(&gb, off);
    adts->objecttype        = m4ac.object_type - 1;
    adts->sample_rate_index = m4ac.sampling_index;
    adts->channel_conf      = m4ac.chan_config;

    if (adts->objecttype > 3U) {
        av_log(s, AV_LOG_ERROR, "MPEG-4 AOT %d is not allowed in ADTS\n", adts->objecttype+1);
        return -1;
    }
    if (adts->sample_rate_index == 15) {
        av_log(s, AV_LOG_ERROR, "Escape sample rate index illegal in ADTS\n");
        return -1;
    }
    if (av_bitstream_get(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "960/120 MDCT window is not allowed in ADTS\n");
        return -1;
    }
    if (av_bitstream_get(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "Scalable configurations are not allowed in ADTS\n");
        return -1;
    }
    if (av_bitstream_get(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "Extension flag is not allowed in ADTS\n");
        return -1;
    }
    if (!adts->channel_conf) {
        av_bitstream_put_init(&pb, adts->pce_data, MAX_PCE_SIZE);

        av_bitstream_put(&pb, 3, 5); //ID_PCE
        adts->pce_size = (avpriv_copy_pce_data(&pb, &gb) + 3) / 8;
        av_bitstream_put_flush(&pb);
    }

    adts->write_adts = 1;

    return 0;
}

static int adts_write_header(AVFormatContext *s)
{
    ADTSContext *adts = s->priv_data;
    AVCodecContext *avc = s->streams[0]->codec;

    if (avc->extradata_size > 0 &&
            adts_decode_extradata(s, adts, avc->extradata, avc->extradata_size) < 0)
        return -1;

    return 0;
}

static int adts_write_frame_header(ADTSContext *ctx,
                                   uint8_t *buf, int size, int pce_size)
{
    AVPutBitContext pb;

    unsigned full_frame_size = (unsigned)ADTS_HEADER_SIZE + size + pce_size;
    if (full_frame_size > ADTS_MAX_FRAME_BYTES) {
        av_log(NULL, AV_LOG_ERROR, "ADTS frame size too large: %u (max %d)\n",
               full_frame_size, ADTS_MAX_FRAME_BYTES);
        return AVERROR_INVALIDDATA;
    }

    av_bitstream_put_init(&pb, buf, ADTS_HEADER_SIZE);

    /* adts_fixed_header */
    av_bitstream_put(&pb, 12, 0xfff);   /* syncword */
    av_bitstream_put(&pb, 1, 0);        /* ID */
    av_bitstream_put(&pb, 2, 0);        /* layer */
    av_bitstream_put(&pb, 1, 1);        /* protection_absent */
    av_bitstream_put(&pb, 2, ctx->objecttype); /* profile_objecttype */
    av_bitstream_put(&pb, 4, ctx->sample_rate_index);
    av_bitstream_put(&pb, 1, 0);        /* private_bit */
    av_bitstream_put(&pb, 3, ctx->channel_conf); /* channel_configuration */
    av_bitstream_put(&pb, 1, 0);        /* original_copy */
    av_bitstream_put(&pb, 1, 0);        /* home */

    /* adts_variable_header */
    av_bitstream_put(&pb, 1, 0);        /* copyright_identification_bit */
    av_bitstream_put(&pb, 1, 0);        /* copyright_identification_start */
    av_bitstream_put(&pb, 13, full_frame_size); /* aac_frame_length */
    av_bitstream_put(&pb, 11, 0x7ff);   /* adts_buffer_fullness */
    av_bitstream_put(&pb, 2, 0);        /* number_of_raw_data_blocks_in_frame */

    av_bitstream_put_flush(&pb);

    return 0;
}

static int adts_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ADTSContext *adts = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t buf[ADTS_HEADER_SIZE];

    if (!pkt->size)
        return 0;
    if (adts->write_adts) {
        int err = adts_write_frame_header(adts, buf, pkt->size,
                                             adts->pce_size);
        if (err < 0)
            return err;
        avio_write(pb, buf, ADTS_HEADER_SIZE);
        if (adts->pce_size) {
            avio_write(pb, adts->pce_data, adts->pce_size);
            adts->pce_size = 0;
        }
    }
    avio_write(pb, pkt->data, pkt->size);

    return 0;
}

AVOutputFormat ff_adts_muxer = {
    .name              = "adts",
    .long_name         = NULL_IF_CONFIG_SMALL("ADTS AAC (Advanced Audio Coding)"),
    .mime_type         = "audio/aac",
    .extensions        = "aac,adts",
    .priv_data_size    = sizeof(ADTSContext),
    .audio_codec       = AV_CODEC_ID_AAC,
    .video_codec       = AV_CODEC_ID_NONE,
    .write_header      = adts_write_header,
    .write_packet      = adts_write_packet,
};
