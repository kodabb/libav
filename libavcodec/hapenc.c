/*
 * VidVox Hap encoder
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
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

/* A fixed header size allows to skip a memcpy */
#define HEADER_SIZE 8

static void compress_texture(AVCodecContext *avctx, const AVFrame *f)
{
    HAPContext *ctx = avctx->priv_data;
    uint8_t *out = ctx->tex_buf;
    int i, j;

    for (j = 0; j < avctx->height; j += 4) {
        for (i = 0; i < avctx->width; i += 4) {
            uint8_t *p = f->data[0] + i * PIXEL_SIZE + j * f->linesize[0];
            const int step = ctx->tex_fun(out, f->linesize[0], p);
            out += step;
        }
    }
}

static int hap_encode(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame, int *got_packet)
{
    HAPContext *ctx = avctx->priv_data;
    size_t final_size = ctx->max_snappy;
    int ret, comp = COMP_SNAPPY;
    int pktsize = FFMAX(ctx->tex_size, ctx->max_snappy) + HEADER_SIZE;

    /* Allocate maximum size packet, shrink later */
    ret = ff_alloc_packet(pkt, pktsize);
    if (ret < 0)
        return ret;

    /* DXTC compression */
    compress_texture(avctx, frame);

    /* Compress with snappy too, write directly on packet buffer */
    ret = snappy_compress(ctx->tex_buf, ctx->tex_size,
                          pkt->data + HEADER_SIZE, &final_size);
    if (ret != SNAPPY_OK) {
        av_log(avctx, AV_LOG_ERROR, "Snappy compress error.\n");
        return AVERROR_BUG;
    }

    /* If there is no gain from snappy, just use the raw texture */
    if (final_size > ctx->tex_size) {
        comp = COMP_NONE;
        av_log(avctx, AV_LOG_WARNING,
               "Snappy buffer bigger than uncompressed (%lu > %lu bytes).\n",
               final_size, ctx->tex_size);
        memcpy(pkt->data + HEADER_SIZE, ctx->tex_buf, ctx->tex_size);
        final_size = ctx->tex_size;
    }

    /* Write header at the start */
    AV_WL24(pkt->data, 0);
    AV_WL32(pkt->data + 4, final_size);
    pkt->data[3] = comp | ctx->section_type;

    av_shrink_packet(pkt, final_size + HEADER_SIZE);
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
    ctx->tex_size   = avctx->width * avctx->height * PIXEL_SIZE / ratio;
    ctx->max_snappy = snappy_max_compressed_length(ctx->tex_size);

    ctx->tex_buf  = av_malloc(ctx->tex_size);
    if (!ctx->tex_buf)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold int hap_close(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;

    av_freep(&ctx->tex_buf);

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
    .long_name      = NULL_IF_CONFIG_SMALL("VidVox HAP encoder"),
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
