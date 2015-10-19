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
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"

typedef struct LibDaalaContext {
    daala_dec_ctx *decoder;
} LibDaalaContext;

static int libdaala_decode(AVCodecContext *avctx, void *data,
                           int *got_frame, AVPacket *avpkt)
{
    LibDaalaContext *ctx = avctx->priv_data;
    AVFrame *frame = data;
    const uint8_t *src_data[4];
    int src_linesizes[4];
    int ret, i;
    od_img img;
    daala_packet dpkt;

    /* Init input/output structures */
    memset(&img, 0, sizeof(img));
    memset(&dpkt, 0, sizeof(dpkt));
    dpkt.packet = avpkt->data;
    dpkt.bytes  = avpkt->size;

    /* Decode */
    ret = daala_decode_packet_in(ctx->decoder, &img, &dpkt);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Decoding error (err %d)\n", ret);
        return AVERROR_INVALIDDATA;
    }

    /* Create output frame */
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    /* Copy decoded data to output frame */
    for (i = 0; i < 4; i++) {
        src_data[i] = img.planes[i].data;
        src_linesizes[i] = img.planes[i].ystride;
    }
    av_image_copy(frame->data, frame->linesize, src_data, src_linesizes,
                  frame->format, frame->width, frame->height);

    /* Frame is ready */
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
    daala_setup_info *setup = NULL;
    daala_info info = { 0 };
    daala_comment comment = { 0 };
    daala_packet dpkt = { 0 };
    int offset, i;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    if (!avctx->extradata) {
        av_log(avctx, AV_LOG_ERROR, "Missing extradata information.\n");
        return AVERROR_INVALIDDATA;
    }

    /* Parse the three headers from extradata */
    daala_info_init(&info);
    for (i = 0, offset = 0; i < 3; i++) {
        offset += dpkt.bytes + 2;
        if (offset >= avctx->extradata_size) {
            av_log(avctx, AV_LOG_ERROR, "Invalid extradata size (%d >= %d).\n",
                   offset, avctx->extradata_size);
            return AVERROR_INVALIDDATA;
        }

        dpkt.packet = avctx->extradata + offset;
        dpkt.bytes = AV_RB16(avctx->extradata + offset - 2);
        dpkt.b_o_s = 1;

        ret = daala_decode_header_in(&info, &comment, &setup, &dpkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error decoding headers.\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (info.bitdepth_mode == OD_BITDEPTH_MODE_8)
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    else if (info.bitdepth_mode == OD_BITDEPTH_MODE_10)
        avctx->pix_fmt = AV_PIX_FMT_YUV420P10;
    else {
        av_log(avctx, AV_LOG_ERROR, "Unsupported bitdepth %d.\n",
               info.bitdepth_mode);
        ret = AVERROR_INVALIDDATA;
        goto out;
    }

    ctx->decoder = daala_decode_create(&info, setup);
    if (!ctx->decoder) {
        av_log(avctx, AV_LOG_ERROR, "Invalid decoder parameters.\n");
        ret = AVERROR_INVALIDDATA;
        goto out;
    }

out:
    daala_comment_clear(&comment);
    daala_info_clear(&info);
    daala_setup_free(setup);

    return ret;
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
