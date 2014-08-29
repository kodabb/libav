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
 * INSERT DESCRIPTION HERE
 */

#include <string.h>

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define SHIFT_LEFT  -1
#define SHIFT_NONE   0
#define SHIFT_RIGHT  1

typedef struct FrameList {
    AVFrame *frame;
    struct FrameList *next;
    int ready;  // a frame is ready when width columns have been processed
} FrameList;

typedef struct TiltandshiftContext {
    const AVClass *class;

    int direction;
    int black;

    int list_size;
    FrameList *input;
} TiltandshiftContext;

static int list_add_frame(TiltandshiftContext *s, AVFrame *frame)
{
    FrameList *element = av_mallocz(sizeof(FrameList));
    if (!element)
        return AVERROR(ENOMEM);

    element->frame = frame;

    if (s->input == NULL) {
        s->input = element;
    } else {
        FrameList *head = s->input;
        while (head->next)
            head = head->next;
        head->next = element;
    }

    s->list_size++;
    return 0;
}

static void list_remove_head(TiltandshiftContext *s)
{
    FrameList *head = s->input;

    if (head) {
        s->input = head->next;
        av_freep(&head);
    }

    s->list_size--;
}

static void list_empty(TiltandshiftContext *s)
{
    FrameList *next = s->input;
    FrameList *tmp;

    while (next) {
        tmp = next;
        next = tmp->next;
        av_free(tmp);
    }
    s->input = NULL;
    s->list_size = 0;
}

static const enum AVPixelFormat formats_supported[] = {
    AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV410P,  AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    ff_set_common_formats(ctx, ff_make_format_list(formats_supported));
    return 0;
}

static av_cold void tiltandshift_uninit(AVFilterContext *ctx)
{
    TiltandshiftContext *s = ctx->priv;
    list_empty(s);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    TiltandshiftContext *s = inlink->dst->priv;
    return list_add_frame(s, frame);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TiltandshiftContext *s = ctx->priv;
    int ret;
    int ncol;
    FrameList *head;
    AVFrame *dst;

    while (s->list_size < outlink->w) {
        ret = ff_request_frame(ctx->inputs[0]);
        if (ret < 0) //TODO handle eof
            return ret;
    }

    dst = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!dst)
        return AVERROR(ENOMEM);
    head = s->input;
    for (ncol = 0; ncol < s->list_size; ncol++) {
        uint8_t *dst_data[4];
        const uint8_t *src_data[4];
        AVFrame *src = head->frame;

        dst_data[0] = dst->data[0] + ncol;
        dst_data[1] = dst->data[1] + ncol / 2;
        dst_data[2] = dst->data[2] + ncol / 2;
        dst_data[3] = dst->data[3];
        src_data[0] = src->data[0] + ncol;
        src_data[1] = src->data[1] + ncol / 2;
        src_data[2] = src->data[2] + ncol / 2;
        src_data[3] = src->data[3];

        av_image_copy(dst_data, dst->linesize,
                      src_data, src->linesize,
                      outlink->format, 1, outlink->h);
        head = head->next;
    }
    ret = av_frame_copy_props(dst, s->input->frame);
    if (ret < 0)
        return ret;

    list_remove_head(s);

    return ff_filter_frame(outlink, dst);
}

#define OFFSET(x) offsetof(TiltandshiftContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "shift", "Shift the input video while tilting", OFFSET(direction), AV_OPT_TYPE_INT,
        { .i64 = SHIFT_LEFT }, SHIFT_LEFT, SHIFT_RIGHT, .flags = V, .unit = "shift" },
    { "left", "shift leftward (default)", 0, AV_OPT_TYPE_CONST,
        { .i64 = SHIFT_LEFT }, INT_MIN, INT_MAX, .flags = V, .unit = "shift" },
    { "right", "shift rightward", 0, AV_OPT_TYPE_CONST,
        { .i64 = SHIFT_RIGHT }, INT_MIN, INT_MAX, .flags = V, .unit = "shift" },
    { "none", "no shift, one image only", 0, AV_OPT_TYPE_CONST,
        { .i64 = SHIFT_NONE }, INT_MIN, INT_MAX, .flags = V, .unit = "shift" },

    { "end", "Allow tilting black at the end", OFFSET(black), AV_OPT_TYPE_INT,
        { .i64 = 1 }, 0, 1, .flags = V, .unit = "end" },

    { NULL },
};

static const AVClass tiltandshift_class = {
    .class_name = "tiltandshift",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad tiltandshift_inputs[] = {
    {
        .name         = "in",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .needs_fifo   = 1,
    },
    { NULL }
};

static const AVFilterPad tiltandshift_outputs[] = {
    {
        .name          = "out",
        .type          = AVMEDIA_TYPE_VIDEO,
      //  .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_tiltandshift = {
    .name          = "tiltandshift",
    .description   = NULL_IF_CONFIG_SMALL("Generate a tilt-and-shift style video."),
    .priv_size     = sizeof(TiltandshiftContext),
    .priv_class    = &tiltandshift_class,
    .query_formats = query_formats,
    .inputs        = tiltandshift_inputs,
    .outputs       = tiltandshift_outputs,
    .uninit        = tiltandshift_uninit,
};
