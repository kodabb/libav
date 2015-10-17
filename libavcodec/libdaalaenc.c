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
#include <string.h>

#include "daala/daalaenc.h"

#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"

typedef struct LibDaalaContext {
    AVClass *class;

    int opt;
    daala_enc_ctx *encoder;
} LibDaalaContext;

/* Concatenate a daala_packet into extradata. */
static int concatenate_packet(AVCodecContext* avctx,
                              const daala_packet* packet,
                              unsigned int *offset)
{
    const char *message = NULL;
    int newsize = avctx->extradata_size + 2 + packet->bytes;
    int ret = AVERROR_INVALIDDATA;

    if (packet->bytes < 0) {
        message = "packet has negative size";
    } else if (packet->bytes > 0xffff) {
        message = "packet is larger than 65535 bytes";
    } else if (newsize < avctx->extradata_size) {
        message = "extradata_size would overflow";
    } else {
        ret = av_reallocp(&avctx->extradata, newsize);
        if (ret < 0) {
            avctx->extradata_size = 0;
            message = "av_realloc failed";
        }
    }
    if (message) {
        av_log(avctx, AV_LOG_ERROR,
               "concatenate_packet failed: %s\n", message);
        return ret;
    }

    avctx->extradata_size = newsize;
    AV_WB16(avctx->extradata + *offset, packet->bytes);
    *offset += 2;
    memcpy(avctx->extradata + *offset, packet->packet, packet->bytes);
    *offset += packet->bytes;

    return 0;
}

static int libdaala_encode(AVCodecContext *avctx, AVPacket *avpkt,
                           const AVFrame *frame, int *got_packet)
{
    LibDaalaContext *ctx = avctx->priv_data;
    int i, ret;
    od_img img;
    daala_packet dpkt;
    memset(&img, 0, sizeof(img));
    memset(&dpkt, 0, sizeof(dpkt));

    img.nplanes = 3;
    img.width   = frame->width;
    img.height  = frame->height;
    for (i = 0; i < img.nplanes; i++) {
        img.planes[i].data = frame->data[i];
        img.planes[i].xstride = avctx->bits_per_coded_sample > 8 ? 1 : 2;
        img.planes[i].ystride = frame->linesize[i];
        img.planes[i].xdec = !!i;
        img.planes[i].ydec = !!i;
    }

    ret = daala_encode_img_in(ctx->encoder, &img, 0);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Cannot accept this frame (err %d)\n", ret);
        return AVERROR_INVALIDDATA;
    }

    // loop because you might have multiple packets in the future
    do {
        ret = daala_encode_packet_out(ctx->encoder, 0, &dpkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Encoding error (err %d)\n", ret);
            return AVERROR_INVALIDDATA;
        }
    } while (ret);

    ret = ff_alloc_packet(avpkt, dpkt.bytes);
    if (ret < 0)
        return ret;
    memcpy(avpkt->data, dpkt.packet, dpkt.bytes);

    // maybe the memcpy above will distract people from this
    avpkt->pts = avpkt->dts = frame->pts;

    if (daala_packet_iskeyframe(dpkt.packet, dpkt.bytes))
        avpkt->flags |= AV_PKT_FLAG_KEY;

    *got_packet = 1;
    return 0;
}

static av_cold int libdaala_init(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;
    daala_comment comment = { 0 };
    daala_info info = { 0 };
    daala_packet dpkt = { 0 };
    int offset = 0;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    daala_info_init(&info);

    av_log(avctx, AV_LOG_VERBOSE, "libdaala version %d.%d.%d\n",
           info.version_major, info.version_minor, info.version_sub);

    info.pic_width  = avctx->width;
    info.pic_height = avctx->height;

    /* Default bitdepth is 8 */
    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10)
        info.bitdepth_mode = OD_BITDEPTH_MODE_10;
    info.nplanes = 3;
    info.plane_info[1].xdec = 1;
    info.plane_info[1].ydec = 1;
    info.plane_info[2].xdec = 1;
    info.plane_info[2].ydec = 1;

    info.timebase_numerator   = avctx->time_base.num;
    info.timebase_denominator = avctx->time_base.den;
    info.frame_duration = 1;
    info.keyframe_rate = avctx->gop_size;

    info.pixel_aspect_numerator   = avctx->sample_aspect_ratio.num;
    info.pixel_aspect_denominator = avctx->sample_aspect_ratio.den;

    ctx->encoder = daala_encode_create(&info);
    if (!ctx->encoder) {
        av_log(avctx, AV_LOG_ERROR, "Invalid encoder parameters.\n");
        return AVERROR_INVALIDDATA;
    }
    daala_comment_init(&comment);
    // various daala_encode_ctl

    while (daala_encode_flush_header(ctx->encoder, &comment, &dpkt)) {
        ret = concatenate_packet(avctx, &dpkt, &offset);
        if (ret < 0)
            return ret;
    }

    daala_comment_clear(&comment);
    return 0;
}

static av_cold int libdaala_close(AVCodecContext *avctx)
{
    LibDaalaContext *ctx = avctx->priv_data;

    daala_encode_free(ctx->encoder);

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
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_NONE,
    },
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
