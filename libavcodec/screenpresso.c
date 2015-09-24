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
 * Fourcc: TSDC
 *
 * Screenpresso is very simple. It codes picture by tiles, storing them in raw BGR24
 * format or compressing them in JPEG. Frames can be full pictures or just
 * updates to the previous frame. Cursor is found in its own frame or at the
 * bottom of the picture. Every frame is then packed with zlib.
 *
 * Supports: BGR24
 */

#include <stdint.h>
#include <zlib.h>

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

#define BITMAPINFOHEADER_SIZE 0x28
#define TDSF_HEADER_SIZE      0x56
#define TDSB_HEADER_SIZE      0x08

typedef struct ScreenpressoContext {
    int width, height;
    GetByteContext gbc;

    AVFrame *refframe;          // full decoded frame (without cursor)
    AVFrame *jpgframe;          // decoded JPEG tile
    uint8_t *tilebuffer;        // buffer containing tile data

    /* zlib interation */
    uint8_t *deflatebuffer;
    uLongf deflatelen;

    /* All that is cursor */
    uint8_t    *cursor;
    int        cursor_stride;
    int        cursor_w, cursor_h, cursor_x, cursor_y;
    int        cursor_hot_x, cursor_hot_y;
} ScreenpressoContext;

/* 1 byte bits, 1 byte planes, 2 bytes format (probably) */
enum ScreenpressoCursorFormat {
    CUR_FMT_MONO = 0x01010004,
    CUR_FMT_BGRA = 0x20010004,
    CUR_FMT_RGBA = 0x20010008,
};

static av_cold int screenpresso_close(AVCodecContext *avctx)
{
    ScreenpressoContext *ctx = avctx->priv_data;

    av_frame_free(&ctx->refframe);
    av_frame_free(&ctx->jpgframe);
    av_freep(&ctx->deflatebuffer);
    av_freep(&ctx->tilebuffer);
    av_freep(&ctx->cursor);

    return 0;
}

static av_cold int screenpresso_init(AVCodecContext *avctx)
{
    ScreenpressoContext *ctx = avctx->priv_data;

    /* These needs to be set to estimate buffer and frame size. */
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* This value should be large enough for a RGB24 frame. */
    ctx->deflatelen = avctx->width * avctx->height * 3;
    ret = av_reallocp(&ctx->deflatebuffer, ctx->deflatelen);
    if (ret < 0)
        return ret;

    /* Allocate reference and JPEG frame */
    ctx->refframe = av_frame_alloc();
    ctx->jpgframe = av_frame_alloc();
    if (!ctx->refframe || !ctx->jpgframe)
        return AVERROR(ENOMEM);

    /* Set the output pixel format on the reference frame */
    ctx->refframe->format = avctx->pix_fmt = AV_PIX_FMT_BGR24;

    return 0;
}

static int screenpresso_decode_frame(AVCodecContext *avctx, void *data,
                                     int *got_frame, AVPacket *avpkt)
{
    ScreenpressoContext *ctx = avctx->priv_data;
    AVFrame *frame = data;
    int ret, keyframe = 0;
    uLongf dlen;

    /* Resize deflate buffer on resolution change */
    /*if (ctx->width != avctx->width || ctx->height != avctx->height) {
        ctx->deflatelen = avctx->width * avctx->height * 3;
        ret = av_reallocp(&ctx->deflatebuffer, ctx->deflatelen);
        if (ret < 0)
            return ret;
    }*/
    dlen = ctx->deflatelen;

    /* Frames are deflated after a 2 byte header, need to inflate them first */
    ret = uncompress(ctx->deflatebuffer, &dlen, avpkt->data + 2, avpkt->size - 2);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Deflate error %d.\n", ret);
        return AVERROR_UNKNOWN;
    }

    /* Get the output frame and copy the reference frame */
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    av_image_copy_plane(frame->data[0], frame->linesize[0],
                        ctx->deflatebuffer, avctx->width * 3,
                        avctx->width * 3, avctx->height);

    /* Frame is ready to be output */
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
