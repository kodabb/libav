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
 * Local threshold filter.
 */

#include <string.h>

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct ThresholdContext {
    const AVClass *class;

    int level;
} ThresholdContext;

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    ff_set_common_formats(ctx, ff_make_format_list(formats_supported));
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    int i, j;
    ThresholdContext *s   = inlink->dst->priv;
    AVFilterContext *ctx  = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    const int range = frame->color_range == AVCOL_RANGE_JPEG ? 255 : 235 - 16;
    const int level = s->level * range / 100;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    if (!desc)
        return AVERROR_BUG;

    for (i = 0; i < frame->height; i++) {
        uint8_t *srcp = frame->data[0] + frame->linesize[0] * i;
        for (j = 0; j < frame->width; j++)
            srcp[j] = srcp[j] > level ? 0xFF : 0x00;
    }
    memset(frame->data[1], 127,
           frame->linesize[1] * frame->height >> desc->log2_chroma_h);
    memset(frame->data[2], 127,
           frame->linesize[2] * frame->height >> desc->log2_chroma_h);

    return ff_filter_frame(outlink, frame);
}

#define OFFSET(x) offsetof(ThresholdContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "level", "Percentage of threshold value", OFFSET(level),
        AV_OPT_TYPE_INT, { .i64 = 60 }, 0, 100, .flags = V },
    { NULL },
};

static const AVClass threshold_class = {
    .class_name = "threshold",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad threshold_input[] = {
    {
        .name           = "in",
        .type           = AVMEDIA_TYPE_VIDEO,
        .filter_frame   = filter_frame,
        .needs_writable = 1,
    },
    { NULL }
};

static const AVFilterPad threshold_output[] = {
    {
        .name          = "out",
        .type          = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_threshold = {
    .name          = "threshold",
    .description   = NULL_IF_CONFIG_SMALL("Color pixels black or white depending on a threshold value."),
    .priv_size     = sizeof(ThresholdContext),
    .priv_class    = &threshold_class,
    .query_formats = query_formats,
    .inputs        = threshold_input,
    .outputs       = threshold_output,
};
