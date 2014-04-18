/*
 * LATM/LOAS muxer
 * Copyright (c) 2011 Kieran Kunhya <kieran@kunhya.com>
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
#include "libavutil/opt.h"
#include "avformat.h"

typedef struct {
    AVClass *av_class;
    int off;
    int channel_conf;
    int object_type;
    int counter;
    int mod;
} LATMContext;

static const AVOption options[] = {
    {"smc-interval", "StreamMuxConfig interval.",
     offsetof(LATMContext, mod), AV_OPT_TYPE_INT, {.i64 = 0x0014}, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM},
    {NULL},
};

static const AVClass latm_muxer_class = {
    .class_name = "LATM/LOAS muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int latm_decode_extradata(LATMContext *ctx, uint8_t *buf, int size)
{
    AVGetBitContext gb;
    MPEG4AudioConfig m4ac;

    av_bitstream_get_init(&gb, buf, size * 8);
    ctx->off = avpriv_mpeg4audio_get_config(&m4ac, buf, size * 8, 1);
    if (ctx->off < 0)
        return ctx->off;
    av_bitstream_skip_long(&gb, ctx->off);

    /* FIXME: are any formats not allowed in LATM? */

    if (m4ac.object_type > AOT_SBR && m4ac.object_type != AOT_ALS) {
        av_log(ctx, AV_LOG_ERROR, "Muxing MPEG-4 AOT %d in LATM is not supported\n", m4ac.object_type);
        return AVERROR_INVALIDDATA;
    }
    ctx->channel_conf = m4ac.chan_config;
    ctx->object_type  = m4ac.object_type;

    return 0;
}

static int latm_write_header(AVFormatContext *s)
{
    LATMContext *ctx = s->priv_data;
    AVCodecContext *avctx = s->streams[0]->codec;

    if (avctx->extradata_size > 0 &&
        latm_decode_extradata(ctx, avctx->extradata, avctx->extradata_size) < 0)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int latm_write_frame_header(AVFormatContext *s, AVPutBitContext *bs)
{
    LATMContext *ctx = s->priv_data;
    AVCodecContext *avctx = s->streams[0]->codec;
    AVGetBitContext gb;
    int header_size;

    /* AudioMuxElement */
    av_bitstream_put(bs, 1, !!ctx->counter);

    if (!ctx->counter) {
        av_bitstream_get_init(&gb, avctx->extradata, avctx->extradata_size * 8);

        /* StreamMuxConfig */
        av_bitstream_put(bs, 1, 0); /* audioMuxVersion */
        av_bitstream_put(bs, 1, 1); /* allStreamsSameTimeFraming */
        av_bitstream_put(bs, 6, 0); /* numSubFrames */
        av_bitstream_put(bs, 4, 0); /* numProgram */
        av_bitstream_put(bs, 3, 0); /* numLayer */

        /* AudioSpecificConfig */
        if (ctx->object_type == AOT_ALS) {
            header_size = avctx->extradata_size-(ctx->off + 7) >> 3;
            avpriv_copy_bits(bs, &avctx->extradata[ctx->off], header_size);
        } else {
            avpriv_copy_bits(bs, avctx->extradata, ctx->off + 3);

            if (!ctx->channel_conf) {
                avpriv_copy_pce_data(bs, &gb);
            }
        }

        av_bitstream_put(bs, 3, 0); /* frameLengthType */
        av_bitstream_put(bs, 8, 0xff); /* latmBufferFullness */

        av_bitstream_put(bs, 1, 0); /* otherDataPresent */
        av_bitstream_put(bs, 1, 0); /* crcCheckPresent */
    }

    ctx->counter++;
    ctx->counter %= ctx->mod;

    return 0;
}

static int latm_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    AVPutBitContext bs;
    int i, len;
    uint8_t loas_header[] = "\x56\xe0\x00";
    uint8_t *buf;

    if (pkt->size > 2 && pkt->data[0] == 0xff && (pkt->data[1] >> 4) == 0xf) {
        av_log(s, AV_LOG_ERROR, "ADTS header detected - ADTS will not be incorrectly muxed into LATM\n");
        return AVERROR_INVALIDDATA;
    }

    buf = av_malloc(pkt->size+1024);
    if (!buf)
        return AVERROR(ENOMEM);

    init_av_bitstream_put(&bs, buf, pkt->size+1024);

    latm_write_frame_header(s, &bs);

    /* PayloadLengthInfo() */
    for (i = 0; i <= pkt->size-255; i+=255)
        av_bitstream_put(&bs, 8, 255);

    av_bitstream_put(&bs, 8, pkt->size-i);

    /* The LATM payload is written unaligned */

    /* PayloadMux() */
    for (i = 0; i < pkt->size; i++)
        av_bitstream_put(&bs, 8, pkt->data[i]);

    av_bitstream_put_align(&bs);
    flush_av_bitstream_put(&bs);

    len = av_bitstream_put_count(&bs) >> 3;

    loas_header[1] |= (len >> 8) & 0x1f;
    loas_header[2] |= len & 0xff;

    avio_write(pb, loas_header, 3);
    avio_write(pb, buf, len);

    av_free(buf);

    return 0;
}

AVOutputFormat ff_latm_muxer = {
    .name           = "latm",
    .long_name      = NULL_IF_CONFIG_SMALL("LOAS/LATM"),
    .mime_type      = "audio/MP4A-LATM",
    .extensions     = "latm",
    .priv_data_size = sizeof(LATMContext),
    .audio_codec    = AV_CODEC_ID_AAC,
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = latm_write_header,
    .write_packet   = latm_write_packet,
    .priv_class     = &latm_muxer_class,
};
