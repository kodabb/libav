/*
 * Apple Pixlet decoder
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

#include <stdint.h>
#include <zlib.h>

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

#define H 0
#define V 1

typedef struct PixletContext {
    GetByteContext gbc;
} PixletContext;

static av_cold int pixlet_close(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;

    return 0;
}

static av_cold int pixlet_init(AVCodecContext *avctx)
{
    PixletContext *ctx = avctx->priv_data;

    // hopefully
    avctx->pix_fmt = AV_PIX_FMT_RGB24;

    return 0;
}

static int pixlet_decode_frame(AVCodecContext *avctx, void *data,
                             int *got_frame, AVPacket *avpkt)
{
    PixletContext *ctx = avctx->priv_data;
    AVFrame *frame = data;
    float *scaling[2];
    unsigned int pktsize;
    int levels, codedplanesize;
    int i, ret;

    bytestream2_init(&ctx->gbc, avpkt->data, avpkt->size);

    pktsize = bytestream2_get_be32(&ctx->gbc);
    if (pktsize - 4 > bytestream2_get_bytes_left(&ctx->gbc)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid packet size %d\n", pktsize);
        return AVERROR_INVALIDDATA;
    }

    if (bytestream2_get_be32(&ctx->gbc) != 1) {
        av_log(avctx, AV_LOG_WARNING, "Only version 1 supported\n");
    }

    bytestream2_skip(&ctx->gbc, 4);
    bytestream2_skip(&ctx->gbc, 4);
    bytestream2_skip(&ctx->gbc, 4);

    avctx->width  = bytestream2_get_be32(&ctx->gbc);
    avctx->height = bytestream2_get_be32(&ctx->gbc);

    levels = bytestream2_get_be32(&ctx->gbc);

    bytestream2_skip(&ctx->gbc, 4);
    bytestream2_skip(&ctx->gbc, 4);

    codedplanesize = bytestream2_get_be32(&ctx->gbc);

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    // 1) read coeffs
    // 2) reconstruct lowpass
    // 3) read highpass
    // 4) combine
    // 5) look at the resulting planes and see if they give sane image in RGB or YUV

    scaling[H] = av_malloc(sizeof(int) * levels);
    scaling[V] = av_malloc(sizeof(int) * levels);
    if (!scaling[0] || !scaling[1])
        return AVERROR(ENOMEM);

    // the main idea is that signal is split into low- and high-pass parts
    // recursively, i.e. L and H; LL, HL, LH and HH; etc and number of times
    // it's applied is called level
    for (i = 0; i < levels; i++) {
        scaling[H][i] = bytestream2_get_be32(&ctx->gbc) / (float) 1000000;
    }
    for (i = 0; i < levels; i++) {
        scaling[V][i] = bytestream2_get_be32(&ctx->gbc) / (float) 1000000;
    }

    *got_frame = 1;

    return 0;
}

AVCodec ff_pixlet_decoder = {
    .name           = "pixlet",
    .long_name      = NULL_IF_CONFIG_SMALL("Apple Pixlet"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_PIXLET,
    .init           = pixlet_init,
    .decode         = pixlet_decode_frame,
    .close          = pixlet_close,
    .priv_data_size = sizeof(PixletContext),
    .capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
