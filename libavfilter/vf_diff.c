/*
 * Copyright (c) 2015 Vittorio Giovara
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
 * Dump image difference on the output frame.
 */

#include <stdlib.h>
#include <string.h>

#include "libavutil/opt.h"
#include "libavutil/rational.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define REF 0
#define CMP 1

typedef struct DiffContext {
    const AVClass *class;

    AVFrame *input_frames[2];            ///< input frames
} DiffContext;

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_RGBA, AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    /* This will ensure that formats are the same on all pads */
    ff_set_common_formats(ctx, ff_make_format_list(formats_supported));
    return 0;
}

static av_cold void diff_uninit(AVFilterContext *ctx)
{
    DiffContext *s = ctx->priv;

    /* clean any leftover frame */
    av_frame_free(&s->input_frames[REF]);
    av_frame_free(&s->input_frames[CMP]);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;

    int width            = ctx->inputs[REF]->w;
    int height           = ctx->inputs[REF]->h;
    AVRational time_base = ctx->inputs[REF]->time_base;

    /* check size and fps match (pixel format always matches) */
    if (width  != ctx->inputs[CMP]->w ||
        height != ctx->inputs[CMP]->h) {
        av_log(ctx, AV_LOG_ERROR,
               "Left and right sizes differ (%dx%d vs %dx%d).\n",
               width, height,
               ctx->inputs[CMP]->w, ctx->inputs[CMP]->h);
        return AVERROR_INVALIDDATA;
    } else if (av_cmp_q(time_base, ctx->inputs[CMP]->time_base) != 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Left and right framerates differ (%d/%d vs %d/%d).\n",
               time_base.num, time_base.den,
               ctx->inputs[CMP]->time_base.num,
               ctx->inputs[CMP]->time_base.den);
        return AVERROR_INVALIDDATA;
    }

    outlink->w         = width;
    outlink->h         = height;
    outlink->time_base = time_base;

    return 0;
}

static int filter_frame_ref(AVFilterLink *inlink, AVFrame *frame)
{
    DiffContext *s = inlink->dst->priv;
    s->input_frames[REF] = frame;
    return 0;
}

static int filter_frame_cmp(AVFilterLink *inlink, AVFrame *frame)
{
    DiffContext *s = inlink->dst->priv;
    s->input_frames[CMP] = frame;
    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DiffContext *s = ctx->priv;
    AVFrame *dst;
    uint8_t *p, *ref, *cmp;
    int ret, i;

    /* get a frame on both input, stop as soon as a video ends */
    for (i = 0; i < 2; i++) {
        if (!s->input_frames[i]) {
            ret = ff_request_frame(ctx->inputs[i]);
            if (ret < 0)
                return ret;
        }
    }

    dst = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!dst)
        return AVERROR(ENOMEM);

    p = dst->data[0];
    ref = s->input_frames[REF]->data[0];
    cmp = s->input_frames[CMP]->data[0];
    for (i = 0; i < outlink->w * 4 * outlink->h; i++)
        p[i] = abs(cmp[i] - ref[i]);

    s->input_frames[REF] = NULL;
    s->input_frames[CMP] = NULL;

    return ff_filter_frame(outlink, dst);
}

#define OFFSET(x) offsetof(DiffContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { NULL },
};

static const AVClass diff_class = {
    .class_name = "diff",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad diff_inputs[] = {
    {
        .name         = "ref",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame_ref,
        .needs_fifo   = 1,
    },
    {
        .name         = "cmp",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame_cmp,
        .needs_fifo   = 1,
    },
    { NULL }
};

static const AVFilterPad diff_outputs[] = {
    {
        .name          = "diff",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_diff = {
    .name          = "diff",
    .description   = NULL_IF_CONFIG_SMALL("Show frame difference visually."),
    .priv_size     = sizeof(DiffContext),
    .priv_class    = &diff_class,
    .query_formats = query_formats,
    .inputs        = diff_inputs,
    .outputs       = diff_outputs,
    .uninit        = diff_uninit,
};
