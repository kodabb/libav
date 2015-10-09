/*
 * libdaala encoder
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

#include "daala/daalaenc.h"

#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "texturedsp.h"

typedef struct LibDaalaContext {
    int opt;
} LibDaalaContext;

static int libdaala_encode(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet)
{
    LibDaalaContext *ctx = avctx->priv_data;
    int ret;
    int pktsize;

    /* Allocate maximum size packet, shrink later. */
    ret = ff_alloc_packet(pkt, pktsize);
    if (ret < 0)
        return ret;

    av_shrink_packet(pkt, pktsize);
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

static av_cold int libdaala_init(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;
    int ratio;
    int corrected_chunk_count;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    return 0;
}

static av_cold int libdaala_close(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;

    return 0;
}

#define OFFSET(x) offsetof(LibDaalaContext, x)
#define FLAGS     AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "opt", "opt", OFFSET(opt), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, FLAGS, },
    { NULL },
};

static const AVClass libdaalaenc_class = {
    .class_name = "libdaala encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libdaala_encoder = {
    .name           = "libdaala",
    .long_name      = NULL_IF_CONFIG_SMALL("libdaala encoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DAALA,
    .priv_data_size = sizeof(LibDaalaContext),
    .priv_class     = &libdaalaenc_class,
    .init           = libdaala_init,
    .encode2        = libdaala_encode,
    .close          = libdaala_close,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE,
    },
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};

