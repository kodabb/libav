/*
 * libdaala decoder
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
#include <string.h>

#include "daala/daaladec.h"

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"

typedef struct LibDaalaContext {
    daala_info info;
    daala_dec_ctx *decoder;
    daala_comment comment;
} LibDaalaContext;

static int libdaala_decode(AVCodecContext *avctx, void *data,
                           int *got_frame, AVPacket *avpkt)
{
    LibDaalaContext *ctx = avctx->priv_data;
    AVFrame *frame = data;
    int ret, i;
    od_img img;
    daala_packet dpkt;
    dpkt.packet = avpkt->data;
    dpkt.bytes  = avpkt->size;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    ret = daala_decode_packet_in(ctx->decoder, &img, &dpkt);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Decoding error (err %d)\n", ret);
        return AVERROR_INVALIDDATA;
    }
    for (i = 0; i < img.nplanes; i++) {
        av_image_copy_plane(frame->data[i], frame->linesize[i],
                            img.planes[i].data, img.planes[i].ystride,
                            frame->linesize[i], frame->height);
    }

    /* Frame is ready to be output */
    if (daala_packet_iskeyframe(dpkt.packet, dpkt.bytes)) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->key_frame = 1;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_P;
    }
    *got_frame = 1;

    return avpkt->size;
}

static av_cold int libdaala_init(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->info.bitdepth_mode = OD_BITDEPTH_MODE_8;
    ctx->info.nplanes = 3;
    ctx->info.plane_info[1].xdec = 1;
    ctx->info.plane_info[1].ydec = 1;
    ctx->info.plane_info[2].xdec = 1;
    ctx->info.plane_info[2].ydec = 1;
    ctx->info.pic_width  = avctx->width;
    ctx->info.pic_height = avctx->height;

    ctx->decoder = daala_decode_alloc(&ctx->info, NULL);
    if (!ctx->decoder) {
        av_log(avctx, AV_LOG_ERROR, "Invalid parameters for decoder\n");
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static av_cold int libdaala_close(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;

    daala_decode_free(ctx->decoder);

    return 0;
}

AVCodec ff_libdaala_decoder = {
    .name           = "libdaala",
    .long_name      = NULL_IF_CONFIG_SMALL("libdaala decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DAALA,
    .init           = libdaala_init,
    .decode         = libdaala_decode,
    .close          = libdaala_close,
    .priv_data_size = sizeof(LibDaalaContext),
    .capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
