/*
 * Copyright (c) 2014 Vittorio Giovara
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
 * Extract one view from a frame-packed video.
 */

#include <string.h>

#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"
#include "libavutil/stereo3d.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define LEFT  0
#define RIGHT 1

typedef struct Stereo2monoContext {
    const AVClass *class;

    enum AVStereo3DType format;     ///< frame pack type intput

    AVFrame *frame;                 ///< input frame

    int view;                       ///< select which view to output

    const AVPixFmtDescriptor *desc; ///< needed when moving pixels

    int frames_in;                  ///< number of received frames
} Stereo2monoContext;

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    ff_set_common_formats(ctx, ff_make_format_list(formats_supported));
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx  = outlink->src;
    Stereo2monoContext *s = outlink->src->priv;

    int width            = ctx->inputs[LEFT]->w;
    int height           = ctx->inputs[LEFT]->h;
    AVRational time_base = ctx->inputs[LEFT]->time_base;

    // check supported format
    if (s->format != AV_STEREO3D_2D &&
        s->format != AV_STEREO3D_SIDEBYSIDE &&
        s->format != AV_STEREO3D_TOPBOTTOM &&
        s->format != AV_STEREO3D_LINES &&
        s->format != AV_STEREO3D_COLUMNS &&
        s->format != AV_STEREO3D_FRAMESEQUENCE) {
        av_log(s, AV_LOG_ERROR, "Unsupported stereo format (%d) requested.\n",
               s->format);
        return AVERROR(EINVAL);
    }
    s->desc = av_pix_fmt_desc_get(outlink->format);
    if (!s->desc)
        return AVERROR_BUG;

    // modify output properties as needed
    switch (s->format) {
    case AV_STEREO3D_FRAMESEQUENCE:
        time_base.num *= 2;
        av_log(NULL, AV_LOG_ERROR, "%d/%d\n", time_base.num, time_base.den);
        break;
    case AV_STEREO3D_COLUMNS:
    case AV_STEREO3D_SIDEBYSIDE:
        width /= 2;
        break;
    case AV_STEREO3D_LINES:
    case AV_STEREO3D_TOPBOTTOM:
        height /= 2;
        break;
    }

    outlink->w         = width;
    outlink->h         = height;
    outlink->time_base = time_base;

    // simplifies later operations
    s->frames_in = -1;

    return 0;
}

static void deframepack(AVFilterLink *outlink, AVFrame *outframe)
{
    AVFilterContext *ctx = outlink->src;
    Stereo2monoContext *s = ctx->priv;
    const uint8_t *src[4];
    int linesizes[4];
    int offset = 0;
    int i;

    memcpy(linesizes, s->frame->linesize, sizeof(linesizes));

    switch (s->format) {
    case AV_STEREO3D_COLUMNS:
        for (i = (s->view == RIGHT); i < s->frame->width; i += 2) {
            uint8_t *dst[4];
            dst[0] = outframe->data[0] + i / 2;
            dst[1] = outframe->data[1] + (i / 2 >> s->desc->log2_chroma_w);
            dst[2] = outframe->data[2] + (i / 2 >> s->desc->log2_chroma_w);
            src[0] = s->frame->data[0] + i;
            src[1] = s->frame->data[1] + (i >> s->desc->log2_chroma_w);
            src[2] = s->frame->data[2] + (i >> s->desc->log2_chroma_w);

            av_image_copy(dst, outframe->linesize, src, linesizes,
                          outlink->format, 1, outlink->h);
        }
        return;
        break;
    case AV_STEREO3D_SIDEBYSIDE:
        if (s->view == RIGHT)
            offset = s->frame->width / 2;

        src[0] = s->frame->data[0] + offset;
        src[1] = s->frame->data[1] + (offset >> s->desc->log2_chroma_w);
        src[2] = s->frame->data[2] + (offset >> s->desc->log2_chroma_w);
        break;
    case AV_STEREO3D_TOPBOTTOM:
        if (s->view == RIGHT)
            offset = s->frame->height / 2;

        src[0] = s->frame->data[0] +
                 s->frame->linesize[0] * offset;
        src[1] = s->frame->data[1] +
                 s->frame->linesize[1] * (offset >> s->desc->log2_chroma_h);
        src[2] = s->frame->data[2] +
                 s->frame->linesize[2] * (offset >> s->desc->log2_chroma_h);
        break;
    case AV_STEREO3D_LINES:
        src[0] = s->frame->data[0] + s->frame->linesize[0] * (s->view == RIGHT);
        src[1] = s->frame->data[1] + s->frame->linesize[1] * (s->view == RIGHT);
        src[2] = s->frame->data[2] + s->frame->linesize[2] * (s->view == RIGHT);

        linesizes[0] = s->frame->linesize[0] * 2;
        linesizes[1] = s->frame->linesize[1] * 2;
        linesizes[2] = s->frame->linesize[2] * 2;
        break;
    }

    av_image_copy(outframe->data, outframe->linesize, src, linesizes,
                  outlink->format, outlink->w, outlink->h);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    Stereo2monoContext *s = inlink->dst->priv;
    s->frames_in++;

    // skip the frame when it's not the requested one
    if (s->format == AV_STEREO3D_FRAMESEQUENCE &&
        ((s->view == LEFT && (s->frames_in & 1)) ||
         (s->view == RIGHT && !(s->frames_in & 1))))
        return AVERROR(EAGAIN);

    s->frame = frame;
    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    Stereo2monoContext *s = ctx->priv;
    AVFrame *dst;
    int ret = ff_request_frame(ctx->inputs[0]);
    if (ret < 0)
        return ret;

    if (s->format == AV_STEREO3D_2D) {
        // passthrough
        dst = s->frame;
    } else if (s->format == AV_STEREO3D_FRAMESEQUENCE) {
        // adjust pts
        s->frame->pts = av_rescale_q(s->frame->pts, ctx->inputs[0]->time_base,
                                     outlink->time_base);
        dst = s->frame;
    } else {
        dst = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!dst)
            return AVERROR(ENOMEM);

        // extract one of the views
        deframepack(outlink, dst);

        // copy any property from the original frame
        ret = av_frame_copy_props(dst, s->frame);
        if (ret < 0) {
            av_frame_free(&s->frame);
            av_frame_free(&dst);
            return ret;
        }

        // frame freedom
        av_frame_free(&s->frame);
    }

    // output
    av_frame_remove_side_data(dst, AV_FRAME_DATA_STEREO3D);
    return ff_filter_frame(outlink, dst);
}

#define OFFSET(x) offsetof(Stereo2monoContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "format", "Frame pack input format (default passthrough)", OFFSET(format), AV_OPT_TYPE_INT,
        { .i64 = AV_STEREO3D_2D }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },
    { "sbs", "Views are packed next to each other", 0, AV_OPT_TYPE_CONST,
        { .i64 = AV_STEREO3D_SIDEBYSIDE }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },
    { "tab", "Views are packed on top of each other", 0, AV_OPT_TYPE_CONST,
        { .i64 = AV_STEREO3D_TOPBOTTOM }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },
    { "frameseq", "Views are one after the other", 0, AV_OPT_TYPE_CONST,
        { .i64 = AV_STEREO3D_FRAMESEQUENCE }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },
    { "lines", "Views are interleaved by lines", 0, AV_OPT_TYPE_CONST,
        { .i64 = AV_STEREO3D_LINES }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },
    { "columns", "Views are interleaved by columns", 0, AV_OPT_TYPE_CONST,
        { .i64 = AV_STEREO3D_COLUMNS }, INT_MIN, INT_MAX, .flags = V, .unit = "format" },

    { "view", "Which view should be output", OFFSET(view), AV_OPT_TYPE_INT,
        { .i64 = LEFT }, 0, INT_MAX, .flags = V, .unit = "view" },
    { "left", "Left view is preserved, right view is discarded", 0, AV_OPT_TYPE_CONST,
        { .i64 = LEFT }, INT_MIN, INT_MAX, .flags = V, .unit = "view" },
    { "right", "Right view is preserved, left view is discarded", 0, AV_OPT_TYPE_CONST,
        { .i64 = RIGHT }, INT_MIN, INT_MAX, .flags = V, .unit = "view" },

    { NULL },
};

static const AVClass stereo2mono_class = {
    .class_name = "stereo2mono",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad stereo2mono_input[] = {
    {
        .name         = "stereo",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .needs_fifo   = 1,
    },
    { NULL }
};

static const AVFilterPad stereo2mono_output[] = {
    {
        .name          = "mono",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_stereo2mono = {
    .name          = "stereo2mono",
    .description   = NULL_IF_CONFIG_SMALL("Extract one view from a stereoscopic pair."),
    .priv_size     = sizeof(Stereo2monoContext),
    .priv_class    = &stereo2mono_class,
    .query_formats = query_formats,
    .inputs        = stereo2mono_input,
    .outputs       = stereo2mono_output,
};
