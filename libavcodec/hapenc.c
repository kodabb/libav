/*
 * Hap video encoder
 * Copyright (c) 2015 Vittorio Giovara
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

/**
 * @file
 * HAP encoder
 *
 * Fourcc: HAP1, HAP5, HAPY
 *
 * https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
 */

#include <stdint.h>
#include "snappy-c.h"

#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "bytestream.h"
#include "hap.h"
#include "internal.h"

static void compress_texture(AVCodecContext *avctx, const AVFrame *frame)
{
    HAPContext *ctx = avctx->priv_data;
    uint8_t *out = ctx->tex_buf;
    int i, j;

    for (j = 0; j < avctx->height; j += 4) {
        for (i = 0; i < avctx->width; i += 4) {
            uint8_t *p = frame->data[0] + i * 4 + j * frame->linesize[0];
            const int step = ctx->tex_fun(out, frame->linesize[0], p);
            out += step;
        }
    }
}

static int hap_encode(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame, int *got_packet)
{
    HAPContext *ctx = avctx->priv_data;
    size_t final_size = ctx->max_snappy;
    int ret, offset, comp;

    /* Allocate maximum size packet, shrink later */
    ret = ff_alloc_packet(pkt, ctx->tex_size + 8);
    if (ret < 0)
        return ret;

    /* DXTC compression */
    compress_texture(avctx, frame);

    /* Compress with snappy too */
    ret = snappy_compress(ctx->tex_buf, ctx->tex_size,
                          ctx->snappied, &final_size);
    if (ret != SNAPPY_OK) {
        av_log(avctx, AV_LOG_ERROR, "Snappy compress error\n");
        return AVERROR_BUG;
    }

    /* If data size does not fit in 3 bytes, keep two long words for
     * the header instead of one. */
    offset = FFMIN(ctx->tex_size, final_size) <= 0x00FFFFFF ? 4 : 8;

    /* If there is no gain from snappy, just use the raw texture */
    if (final_size > ctx->tex_size) {
        comp = COMP_NONE;
        av_log(avctx, AV_LOG_VERBOSE,
               "Snappy buffer bigger than uncompressed (%lu > %lu bytes)\n",
               final_size, ctx->tex_size);
        memcpy(pkt->data + offset, ctx->tex_buf, ctx->tex_size);
        final_size = ctx->tex_size;
    } else {
        comp = COMP_SNAPPY;
        memcpy(pkt->data + offset, ctx->snappied, final_size);
    }

    /* Write header at the start */
    if (offset == 4) {
        AV_WL24(pkt->data, final_size);
    } else {
        AV_WL24(pkt->data, 0);
        AV_WL32(pkt->data + 4, final_size);
    }
    pkt->data[3] = comp | ctx->section_type;

    av_shrink_packet(pkt, final_size + offset);
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

static av_cold int hap_init(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;
    int ratio;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    if (avctx->width % 4 || avctx->height % 4) {
        av_log(avctx, AV_LOG_ERROR, "Video size is not multiple of 4 %dx%d.\n",
               avctx->width, avctx->height);
        return AVERROR(ENOSYS);
    }

    ff_dxtc_compression_init(&ctx->dxtc);

    switch (ctx->section_type & 0x0F) {
    case FMT_RGBDXT1:
        ratio = 8;
        avctx->codec_tag = MKTAG('H', 'a', 'p', '1');
        ctx->tex_fun = ctx->dxtc.dxt1_block;
        break;
    case FMT_RGBADXT5:
        ratio = 4;
        avctx->codec_tag = MKTAG('H', 'a', 'p', '5');
        ctx->tex_fun = ctx->dxtc.dxt5_block;
        break;
    case FMT_YCOCGDXT5:
        ratio = 4;
        avctx->codec_tag = MKTAG('H', 'a', 'p', 'Y');
        ctx->tex_fun = ctx->dxtc.dxt5ys_block;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Invalid format %02X\n", ctx->section_type);
        return AVERROR_INVALIDDATA;
    }

    /* Texture compression ratio is constant, so can we computer
     * beforehand the final size of the uncompressed buffer */
    ctx->tex_size   = avctx->width * avctx->height * 4 / ratio;
    ctx->max_snappy = snappy_max_compressed_length(ctx->tex_size);

    ctx->tex_buf  = av_malloc(ctx->tex_size);
    ctx->snappied = av_malloc(ctx->max_snappy);
    if (!ctx->tex_buf || !ctx->snappied)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold int hap_close(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;

    av_freep(&ctx->tex_buf);
    av_freep(&ctx->snappied);

    return 0;
}

#define OFFSET(x) offsetof(HAPContext, x)
#define FLAGS     AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "profile", NULL, OFFSET(section_type), AV_OPT_TYPE_INT, { .i64 = FMT_RGBDXT1 }, FMT_RGBDXT1, FMT_YCOCGDXT5, FLAGS, "profile" },
        { "hap",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FMT_RGBDXT1   }, 0, 0, FLAGS, "profile" },
        { "hap_alpha", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FMT_RGBADXT5  }, 0, 0, FLAGS, "profile" },
        { "hap_q",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FMT_YCOCGDXT5 }, 0, 0, FLAGS, "profile" },

    { NULL },
};

static const AVClass hapenc_class = {
    .class_name = "Hap encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hap_encoder = {
    .name           = "hap",
    .long_name      = NULL_IF_CONFIG_SMALL("Vidvox HAP encoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HAP,
    .priv_data_size = sizeof(HAPContext),
    .priv_class     = &hapenc_class,
    .init           = hap_init,
    .encode2        = hap_encode,
    .close          = hap_close,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_RGBA, AV_PIX_FMT_NONE
    },
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
