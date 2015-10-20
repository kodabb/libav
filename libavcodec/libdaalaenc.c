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

/*
 * Video quality
 *     Encoding complexity is controlled by setting avctx->compression_level.
 *     The valid range is 0 to 10.  A higher setting gives generally better
 *     quality at the expense of encoding speed.  This does not affect the
 *     bit rate.
 *
 * Complexity
 *     Encoding complexity is controlled by setting avctx->compression_level.
 *     The valid range is 0 to 10.  A higher setting gives generally better
 *     quality at the expense of encoding speed.  This does not affect the
 *     bit rate.
 */

#include <stdint.h>
#include <string.h>

#include "daala/daalaenc.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"

typedef struct LibDaalaContext {
    AVClass *class;
    int dering;
    int mc_satd;
    int mc_chroma;
    int activity_masking;
    int qm;
    int mv_res_min;
    int mv_level_min;
    int mv_level_max;

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
        img.planes[i].xstride = avctx->pix_fmt == AV_PIX_FMT_YUV420P10 ? 2 : 1;
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

    if (daala_packet_iskeyframe(&dpkt))
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
    const int optsize = sizeof(int);
    int offset = 0;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (avctx->strict_std_compliance > FF_COMPLIANCE_EXPERIMENTAL) {
        av_log(avctx, AV_LOG_ERROR,
               "Experimental encoder, set -strict experimental to use it.\n");
        return AVERROR(ENOSYS);
    }

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

    if (avctx->compression_level > FF_COMPRESSION_DEFAULT) {
        int complexity = av_clip(avctx->compression_level, 0, 10);
        daala_encode_ctl(ctx->encoder, OD_SET_COMPLEXITY, &complexity, optsize);
    }

    if (avctx->global_quality != 0) {
        int video_q = av_clip(avctx->global_quality, 0, 511);
        daala_encode_ctl(ctx->encoder, OD_SET_QUANT, &video_q, optsize);
    }

    daala_encode_ctl(ctx->encoder, OD_SET_DERING, &ctx->dering, optsize);
    daala_encode_ctl(ctx->encoder, OD_SET_MC_CHROMA, &ctx->mc_chroma, optsize);
    daala_encode_ctl(ctx->encoder, OD_SET_MC_SATD, &ctx->mc_satd, optsize);

    daala_encode_ctl(ctx->encoder, OD_SET_ACTIVITY_MASKING, &ctx->activity_masking, optsize);
    daala_encode_ctl(ctx->encoder, OD_SET_QM, &ctx->qm, optsize);

    daala_encode_ctl(ctx->encoder, OD_SET_MV_RES_MIN, &ctx->mv_res_min, optsize);

    if (ctx->mv_level_min > ctx->mv_level_max) {
        av_log(avctx, AV_LOG_ERROR, "Invalid mv levels (min: %d > max: %d)\n",
               ctx->mv_level_min, ctx->mv_level_max);
        return AVERROR(EINVAL);
    }
    daala_encode_ctl(ctx->encoder, OD_SET_MV_LEVEL_MIN, &ctx->mv_level_min, optsize);
    daala_encode_ctl(ctx->encoder, OD_SET_MV_LEVEL_MAX, &ctx->mv_level_max, optsize);

    daala_comment_init(&comment);
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
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "dering", "Use dering postprocessing filter", OFFSET(dering), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, VE },
    { "mc-satd", "Use SATD metric in motion compensation", OFFSET(mc_satd), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 1, VE },
    { "mc-chroma", "Use chroma planes in motion compensation", OFFSET(mc_chroma), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, VE },
    { "amask", "Use activity masking in quantization", OFFSET(activity_masking), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, VE },

    { "qm", "Select quantization matrix (0: flat, 1: hvs)", OFFSET(qm), AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 1, VE },
    { "mv-res-min", "Minimum motion vectors resolution for motion compensation search", OFFSET(mv_res_min), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 2, VE, "mvres" },
        { "8pel", "1/8 pel (default)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, VE, "mvres" },
        { "4pel", "1/4 pel", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, VE, "mvres" },
        { "2pel", "1/2 pel", 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, VE, "mvres" },
    { "mv-level-min", "Minimum motion vectors level", OFFSET(mv_level_min), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, 6, VE },
    { "mv-level-max", "Maximum motion vectors level", OFFSET(mv_level_max), AV_OPT_TYPE_INT, {.i64 = 6 }, 0, 6, VE },

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
