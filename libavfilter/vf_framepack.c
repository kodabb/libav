/*
 * Copyright (c) 2013 Vittorio Giovara
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
 * generate a frame packed video
 */

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/rational.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/stereo3d.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define LEFT_VIEW  0
#define RIGHT_VIEW 1

typedef struct {
    const AVClass *class;

    const AVPixFmtDescriptor *pix_desc; ///< the agreed pixel format

    enum AVStereo3DType format;         ///< the frame packed output
    int fullsize;                       ///< whether input need no downsample

    AVFrame *left;                      ///< the left input frame
    AVFrame *right;                     ///< the right input frame
} FramepackContext;

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV410P,  AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_GRAY8,    AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    ff_set_common_formats(ctx, ff_make_format_list(formats_supported));
    return 0;
}

static int config_input_left(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    FramepackContext *s = inlink->dst->priv;

    // check filter input
    if (s->format == AV_STEREO3D_UNKNOWN ||
        s->format == AV_STEREO3D_MULTISTREAM) {
        av_log(ctx, AV_LOG_ERROR, "Selected format is not framepacked\n");
        return AVERROR_INVALIDDATA;
    } else if (s->format == AV_STEREO3D_ANAGLYPH) {
        av_log(ctx, AV_LOG_ERROR, "Anaglyph blending not supported\n");
        return AVERROR_PATCHWELCOME;
    }

    // save pixel format which has to match the other one
    s->pix_desc = av_pix_fmt_desc_get(inlink->format);

    return 0;
}

static int config_input_right(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    FramepackContext *s = inlink->dst->priv;

    // check input format
    if (s->pix_desc != av_pix_fmt_desc_get(inlink->format)) {
        av_log(ctx, AV_LOG_ERROR,
               "Videos' color space differs.\n");
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FramepackContext *s  = outlink->src->priv;

    int width, height;
    AVRational time_base;

    // normal case, same as input
    width     = ctx->inputs[LEFT_VIEW]->w;
    height    = ctx->inputs[LEFT_VIEW]->h;
    time_base = ctx->inputs[LEFT_VIEW]->time_base;

    // check size and fps match on the other input
    if (width  != ctx->inputs[RIGHT_VIEW]->w ||
        height != ctx->inputs[RIGHT_VIEW]->h) {
        av_log(ctx, AV_LOG_ERROR,
               "Videos' size differs.\n");
        return AVERROR_INVALIDDATA;
    } else if (!av_cmp_q(time_base, ctx->inputs[RIGHT_VIEW]->time_base)) {
        av_log(ctx, AV_LOG_ERROR,
               "Videos' frame rate differs.\n");
        return AVERROR_INVALIDDATA;
    }

    // modify output properties as needed
    switch (s->format) {
    case AV_STEREO3D_2D:
        s->fullsize = 1;
        av_log(ctx, AV_LOG_WARNING,
               "No frame packing mode selected\n");
        break;
    case AV_STEREO3D_FRAMESEQUENCE:
        time_base.den *= 2;
        break;
    case AV_STEREO3D_COLUMNS:
    case AV_STEREO3D_SIDEBYSIDE:
        if (s->fullsize)
            width *= 2;
        break;
    case AV_STEREO3D_LINES:
    case AV_STEREO3D_TOPBOTTOM:
        if (s->fullsize)
            height *= 2;
        break;
    case AV_STEREO3D_TILES:
        if (!s->fullsize)
            av_log(ctx, AV_LOG_WARNING,
                   "Cannot downsample for this format.\n");
        width  = 3 * width / 2;
        height = 3 * height / 2;
        s->fullsize = 1;
        break;
    }

    outlink->w         = width;
    outlink->h         = height;
    outlink->time_base = time_base;

    return 0;
}

static AVFrame *pack_topbottom_frame(AVFilterLink *outlink,
                                     AVFrame *left,
                                     AVFrame *right,
                                     int fullsize, int interleaved)
{
    AVFrame *out;

    int step_dst = interleaved * 2;
    int step_src = fullsize ? 1 : 2;

    int plane, i;
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return NULL;

    for (plane = 0; plane < 3; plane++) {
        uint8_t *srcp = left->data[plane];
        uint8_t *dstp;
        int lines  = left->height;
        int stride = out->width >> (plane > 0 ? 1 : 0); //hsub

        if (plane == 1 || plane == 2)
            lines = -(-left->height >> 1);  //vsub

        dstp = out->data[plane];
        for (i = 0; i < lines; i += step_src) {
            memcpy(dstp, srcp, stride);
            dstp += out->linesize[plane] * step_dst;
            srcp += left->linesize[plane] * step_src;
        }
        srcp = right->data[plane];
        // offset 1 line
        if (interleaved) {
            dstp  = out->data[plane];
            dstp += out->linesize[plane];
        }
        for (i = 0; i < lines; i += step_src) {
            memcpy(dstp, srcp, stride);
            dstp += out->linesize[plane] * step_dst;
            srcp += right->linesize[plane] * step_src;
        }
    }

    return out;
}

static AVFrame *pack_sidebyside_frame(AVFilterLink *outlink,
                                      AVFrame *left,
                                      AVFrame *right,
                                      int fullsize, int interleaved)
{
    AVFrame *out;

    int plane, i;
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return NULL;

    for (plane = 0; plane < 3; plane++) {
        const uint8_t *leftp  = left->data[plane];
        const uint8_t *rightp = right->data[plane];
        uint8_t *dstp         = out->data[plane];
        int lines             = left->height;
        int stride            = (out->width >> (plane > 0 ? 1 : 0)) / 2; //hsub

        if (plane == 1 || plane == 2)
            lines = -(-left->height >> 1);  //vsub

        if (fullsize && !interleaved)
            for (i = 0; i < lines; i++) {
                memcpy(dstp, leftp, stride);
                dstp += out->linesize[plane] / 2;
                memcpy(dstp, rightp, stride);
                dstp += out->linesize[plane] / 2;

                leftp  += left->linesize[plane];
                rightp += right->linesize[plane];
            }
        else {
            for (i = 0; i < lines; i++) {
                int j;
                int k = 0;

                if (interleaved)
                    for (j = 0; j < stride * (2 - fullsize); j += 2 - fullsize) {
                        dstp[k++] = leftp[j];
                        dstp[k++] = rightp[j];
                    }
                else {
                    for (j = 0; j < stride * 2; j += 2)
                        dstp[k++] = leftp[j];
                    for (j = 0; j < stride * 2; j += 2)
                        dstp[k++] = rightp[j];
                }

                dstp   += out->linesize[plane];
                leftp  += left->linesize[plane];
                rightp += right->linesize[plane];
            }
        }
    }

    return out;
}

static AVFrame *pack_tiles_frame(AVFilterLink *outlink,
                                 AVFrame *left,
                                 AVFrame *right)
{
    AVFrame *out;
    int plane, i, j;
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return NULL;

    for (plane = 0; plane < 3; plane++) {
        const uint8_t *leftp  = left->data[plane];
        const uint8_t *rightp = right->data[plane];
        uint8_t *dstp;
        int lines  = left->height;
        int stride = out->width >> (plane > 0 ? 1 : 0); //hsub
        int black  = (plane > 0) ? 127 : 0;

        if (plane == 1 || plane == 2) {
            lines = -(-left->height >> 1); //vsub
        }

        dstp = out->data[plane];
        for (i = 0; i < lines; i++) {
            memcpy(dstp, leftp, stride * 2 / 3);
            dstp += stride * 2 / 3;
            memcpy(dstp, rightp, stride / 3);
            dstp += stride / 3;

            leftp  += left->linesize[plane];
            rightp += right->linesize[plane];
        }

        for (i = 0; i < lines / 2; i++) {
            rightp = right->data[plane] + stride / 3 + right->linesize[plane] * i;
            memcpy(dstp, rightp, stride / 3);
            dstp += stride / 3;

            rightp += right->linesize[plane] * lines / 2;
            memcpy(dstp, rightp, stride / 3);
            dstp += stride / 3;

            // free space(
            for (j = 0; j < stride / 3; j++)
                dstp[j] = black;

            dstp += stride / 3;
        }
    }

    return out;
}

static AVFrame *pack_checkers_frame(AVFilterLink *outlink,
                                    AVFrame *left,
                                    AVFrame *right)
{
    AVFrame *out;

    int plane, i;
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return NULL;

    for (plane = 0; plane < 3; plane++) {
        const uint8_t *leftp  = left->data[plane];
        const uint8_t *rightp = right->data[plane];
        uint8_t *dstp         = out->data[plane];
        int lines             = left->height;
        int stride            = (out->width >> (plane > 0 ? 1 : 0)) / 2; //hsub
        int k = 0;

        if (plane == 1 || plane == 2)
            lines = -(-left->height >> 1);  //vsub

        for (i = 0; i < lines; i++) {
            for (k = 0; k < stride * 2; k += 2) {
                dstp[k]     = i % 2 ? rightp[k + 2] : leftp[k + 2];
                dstp[k + 1] = i % 2 ? leftp[k + 2] : rightp[k + 2];
            }

            dstp   += out->linesize[plane];
            leftp  += left->linesize[plane];
            rightp += right->linesize[plane];
        }
    }

    return out;
}

static int filter_frame_left(AVFilterLink *inlink, AVFrame *frame)
{
    FramepackContext *s = inlink->dst->priv;

    s->left = frame;

    return 0;
}

static int filter_frame_right(AVFilterLink *inlink, AVFrame *frame)
{
    FramepackContext *s = inlink->dst->priv;

    s->right = frame;

    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FramepackContext *s  = ctx->priv;
    int ret;
    AVFrame *out;
    static uint64_t double_pts;

    /* get a frame on the left input
     * on EOF reuse previous */
    if (!s->left) {
        ret = ff_request_frame(ctx->inputs[LEFT_VIEW]);
        if (ret < 0)
            return ret;
    }
    /* get a new frame on the right input,
     * on EOF reuse previous */
    if (!s->right) {
        ret = ff_request_frame(ctx->inputs[RIGHT_VIEW]);
        if (ret == AVERROR_EOF)
            exit(-1);
        //return handle_overlay_eof(ctx);
        else if (ret < 0)
            return ret;
    }

    switch (s->format) {
    case AV_STEREO3D_2D:
        out = av_frame_clone(s->left);
        if (!out)
            return AVERROR(ENOMEM);
        break;
    case AV_STEREO3D_FRAMESEQUENCE:
        double_pts = s->left->pts;
        out = av_frame_clone(s->left);
        if (!out)
            return AVERROR(ENOMEM);
        out->pts = double_pts++;
        av_frame_free(&s->left);

        ret = ff_filter_frame(outlink, out);
        if (ret < 0)
            return ret;

        out = av_frame_clone(s->right);
        if (!out)
            return AVERROR(ENOMEM);
        out->pts = double_pts++;
        av_frame_free(&s->right);

        ret = ff_filter_frame(outlink, out);
        return ret;
        break;
    case AV_STEREO3D_CHECKERS:
        out = pack_checkers_frame(outlink, s->left, s->right);
        break;
    case AV_STEREO3D_COLUMNS:
        out = pack_sidebyside_frame(outlink, s->left, s->right, s->fullsize, 1);
        break;
    case AV_STEREO3D_SIDEBYSIDE:
        out = pack_sidebyside_frame(outlink, s->left, s->right, s->fullsize, 0);
        break;
    case AV_STEREO3D_LINES:
        out = pack_topbottom_frame(outlink, s->left, s->right, s->fullsize, 1);
        break;
    case AV_STEREO3D_TOPBOTTOM:
        out = pack_topbottom_frame(outlink, s->left, s->right, s->fullsize, 0);
        break;
    case AV_STEREO3D_TILES:
        out = pack_tiles_frame(outlink, s->left, s->right);
        break;
    }
    if (!out)
        return AVERROR(ENOMEM);

    ret = av_frame_copy_props(out, s->left);
    if (ret < 0)
        return ret;

    av_frame_free(&s->left);
    av_frame_free(&s->right);

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(FramepackContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "format", "Frame pack output format",                OFFSET(format),   AV_OPT_TYPE_INT,
        { .i64 = 0 },                                       0, INT_MAX,          .flags = V },
    { "size",   "Keep original view sizes (no downscale)", OFFSET(fullsize), AV_OPT_TYPE_INT,
        { .i64 = 0 },                                       0,                1, .flags = V },
    { NULL },
};

static const AVClass framepack_class = {
    .class_name = "framepack",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad framepack_inputs[] = {
    {
        .name         = "left",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_left,
        .filter_frame = filter_frame_left,
        .needs_fifo   = 1,
    },
    {
        .name         = "right",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_right,
        .filter_frame = filter_frame_right,
        .needs_fifo   = 1,
    },
    { NULL }
};

static const AVFilterPad framepack_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_framepack = {
    .name          = "framepack",
    .description   = NULL_IF_CONFIG_SMALL("Generate a frame packed stereoscopic video."),
    .priv_size     = sizeof(FramepackContext),
    .priv_class    = &framepack_class,
    .query_formats = query_formats,
    .inputs        = framepack_inputs,
    .outputs       = framepack_outputs,
};
