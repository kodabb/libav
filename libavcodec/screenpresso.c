/*
 * Screenpresso decoder
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
 * Screenpresso decoder
 *
 * Fourcc: SPV1
 *
 * Screenpresso simply horizontally flips and then deflates frames, alternating
 * full pictures and deltas (from the currently rebuilt frame).
 * No coordinate system (or any meaningful header), so a full frame is
 * sent every time, with black for delta frames instead of pixel data.
 *
 * Supports: BGR24
 */

#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"

typedef struct ScreenpressoContext {
    AVFrame *current;

    /* zlib interation */
    uint8_t *deflatebuffer;
    uLongf deflatelen;
} ScreenpressoContext;

static av_cold int screenpresso_close(AVCodecContext *avctx)
{
    ScreenpressoContext *ctx = avctx->priv_data;

    av_frame_free(&ctx->current);
    av_freep(&ctx->deflatebuffer);

    return 0;
}

static av_cold int screenpresso_init(AVCodecContext *avctx)
{
    ScreenpressoContext *ctx = avctx->priv_data;

    /* These needs to be set to estimate uncompressed buffer */
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Allocate current frame */
    ctx->current = av_frame_alloc();
    if (!ctx->current)
        return AVERROR(ENOMEM);

    avctx->pix_fmt = AV_PIX_FMT_BGR24;

    return 0;
}

static void copy_plane_flipped(uint8_t       *dst, int dst_linesize,
                               const uint8_t *src, int src_linesize,
                               int bytewidth, int height)
{
    for (; height > 0; height--) {
        memcpy(dst, src + (height - 1) * src_linesize, bytewidth);
        dst += dst_linesize;
    }
}

static void copy_delta_flipped(uint8_t       *dst, int dst_linesize,
                               const uint8_t *src, int src_linesize,
                               int bytewidth, int height)
{
    int i, j;
    for (i = 0; i < height; i++) {
        for (j = 0; j < bytewidth; j++)
            dst[j] += src[(height - 1 - i) * src_linesize + j];
        dst += dst_linesize;
    }
}

static int screenpresso_decode_frame(AVCodecContext *avctx, void *data,
                                     int *got_frame, AVPacket *avpkt)
{
    ScreenpressoContext *ctx = avctx->priv_data;
    AVFrame *frame = data;
    int keyframe = (avpkt->data[0] == 0x73);
    int ret;

    /* Basic sanity check, but not really harmful */
    if ((avpkt->data[0] != 0x73 && avpkt->data[0] != 0x72) ||
        avpkt->data[1] != 8) { // bpp probably
        av_log(avctx, AV_LOG_WARNING, "Unknown header 0x%02X%02X\n",
               avpkt->data[0], avpkt->data[1]);
    }

    /* Resize deflate buffer and frame on resolution change */
    if (ctx->deflatelen != avctx->width * avctx->height * 3) {
        ret = ff_get_buffer(avctx, ctx->current, 0);
        if (ret < 0)
            return ret;

        ctx->deflatelen = avctx->width * avctx->height * 3;
        ret = av_reallocp(&ctx->deflatebuffer, ctx->deflatelen);
        if (ret < 0)
            return ret;
    }

    /* Skip the 2 byte header, and then inflate the frame */
    ret = uncompress(ctx->deflatebuffer, &ctx->deflatelen,
                     avpkt->data + 2, avpkt->size - 2);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Deflate error %d.\n", ret);
        return AVERROR_UNKNOWN;
    }

    /* When a keyframe is sent copy it as it contains the whole picture */
    if (keyframe)
        copy_plane_flipped(ctx->current->data[0], ctx->current->linesize[0],
                           ctx->deflatebuffer, avctx->width * 3,
                           avctx->width * 3, avctx->height);
    /* Otherwise sum the delta on top of the current frame */
    else
        copy_delta_flipped(ctx->current->data[0], ctx->current->linesize[0],
                           ctx->deflatebuffer, avctx->width * 3,
                           avctx->width * 3, avctx->height);

    /* Frame is ready to be output */
    ret = av_frame_ref(frame, ctx->current);
    if (ret < 0)
        return ret;

    /* Usual properties */
    if (keyframe) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->key_frame = 1;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_P;
    }
    *got_frame = 1;

    return 0;
}

AVCodec ff_screenpresso_decoder = {
    .name           = "screenpresso",
    .long_name      = NULL_IF_CONFIG_SMALL("Screenpresso"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_SCREENPRESSO,
    .init           = screenpresso_init,
    .decode         = screenpresso_decode_frame,
    .close          = screenpresso_close,
    .priv_data_size = sizeof(ScreenpressoContext),
    .capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
