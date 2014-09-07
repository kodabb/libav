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
 * @file vf_tiltandshift.c
 * Simple time and space inverter.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define TILT_NONE  -1
#define TILT_FRAME  0
#define TILT_BLACK  1

typedef struct FrameList {
    AVFrame *frame;
    struct FrameList *next;
} FrameList;

typedef struct TiltandshiftContext {
    const AVClass *class;

    /* set when all input frames have been processed and we have to
     * empty buffers, pad and then return */
    int eof_recv;

    /* live or static sliding */
    int tilt;

    /* initial or final actions to perform (pad/hold a frame/black/nothing) */
    int start;
    int end;

    /* columns to hold or pad at the beginning or at the end (respectively) */
    int hold;
    int pad;

    /* buffers for black columns */
    uint8_t *black_buffers[4];
    int black_linesizes[4];

    /* list containing all input frames */
    int list_size;
    FrameList *input;
    FrameList *prev;
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

static av_cold void uninit(AVFilterContext *ctx)
{
    TiltandshiftContext *s = ctx->priv;
    av_freep(&s->black_buffers);
    list_empty(s);
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TiltandshiftContext *s = ctx->priv;

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->format = ctx->inputs[0]->format;

    // when we have to pad black or a frame at the start, skip navigating
    // the list and use either the frame or black for the requested value
    if (s->start != TILT_NONE && !s->hold)
        s->hold = outlink->w;

    // Init black buffers if we pad with black at the start or at the end.
    // For the end, we always have to init on NONE and BLACK because we never
    // know if there are going to be enough input frames to fill an output one.
    if (s->start == TILT_BLACK || s->end != TILT_FRAME) {
        int i, j, ret;
        uint8_t black_data[] = { 0x10, 0x80, 0x80, 0x10 };
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(outlink->format);
        if (!desc)
            return AVERROR_BUG;

        if (outlink->format == AV_PIX_FMT_YUVJ420P ||
            outlink->format == AV_PIX_FMT_YUVJ422P ||
            outlink->format == AV_PIX_FMT_YUVJ444P ||
            outlink->format == AV_PIX_FMT_YUVJ440P)
            black_data[0] = black_data[3] = 0;

        ret = av_image_alloc(s->black_buffers, s->black_linesizes, 1,
                             outlink->h, outlink->format, 1);
        if (ret < 0)
            return ret;

        for (i = 0; i < FFMIN(desc->nb_components, 4); i++)
            for (j = 0; j < (!i ? outlink->h
                                : -((-outlink->h) >> desc->log2_chroma_h)); j++)
                memset(s->black_buffers[i] + j * s->black_linesizes[i],
                       black_data[i], 1);

        av_log(ctx, AV_LOG_VERBOSE, "Padding buffers initialized.\n");
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    TiltandshiftContext *s = inlink->dst->priv;
    return list_add_frame(s, frame);
}

static void copy_column(uint8_t *dst_data[4], int dst_linesizes[4],
                        const uint8_t *src_data[4], const int src_linesizes[4],
                        enum AVPixelFormat format, int height, int ncol,
                        int tilt)
{
    uint8_t *dst[4];
    const uint8_t *src[4];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    if (!desc)
        return;

    dst[0] = dst_data[0] + ncol;
    dst[1] = dst_data[1] + (ncol >> desc->log2_chroma_h);
    dst[2] = dst_data[2] + (ncol >> desc->log2_chroma_h);

    if (!tilt)
        ncol = 0;
    src[0] = src_data[0] + ncol;
    src[1] = src_data[1] + (ncol >> desc->log2_chroma_h);
    src[2] = src_data[2] + (ncol >> desc->log2_chroma_h);

    av_image_copy(dst, dst_linesizes, src, src_linesizes, format, 1, height);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TiltandshiftContext *s = ctx->priv;
    FrameList *head = s->input;
    int ret, ncol;
    AVFrame *dst;

    // signal job finished when list is empty or when padding is either
    // limited or disabled and eof was received
    if ((s->list_size <= 0 || s->list_size == outlink->w - s->pad ||
         s->end == TILT_NONE) && s->eof_recv)
        return AVERROR_EOF;

    // load up enough frames to fill a frame and keep it filled on subsequent
    // calls, until we receive EOF, and then we either pad or end
    while (!s->eof_recv && s->list_size < outlink->w) {
        ret = ff_request_frame(ctx->inputs[0]);
        if (ret == AVERROR_EOF) {
            av_log(ctx, AV_LOG_VERBOSE, "Last frame, emptying buffers.\n");
            s->eof_recv = 1;
            break;
        }
        if (ret < 0)
            return ret;
    }

    // new frame
    ncol = 0;
    dst = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!dst)
        return AVERROR(ENOMEM);

    // in case we have to do any initial black padding
    if (s->start == TILT_BLACK) {
        for ( ; ncol < s->hold; ncol++)
            copy_column(dst->data, dst->linesize,
                        (const uint8_t **)s->black_buffers, s->black_linesizes,
                        outlink->format, outlink->h, ncol, 0);
    }

    // copy a column from each input frame
    for ( ; ncol < s->list_size; ncol++) {
        AVFrame *src = head->frame;

        copy_column(dst->data, dst->linesize,
                    (const uint8_t **)src->data, src->linesize,
                    outlink->format, outlink->h, ncol, s->tilt);

        // keep track of the last known frame in case we need it below
        s->prev = head;
        // advance to the next frame unless we have to hold it
        if (s->hold <= ncol)
            head = head->next;
    }

    // pad any remaining space with black or last frame
    if (s->end == TILT_FRAME) {
        for ( ; ncol < outlink->w; ncol++)
            copy_column(dst->data, dst->linesize,
                        (const uint8_t **)s->prev->frame->data,
                        s->prev->frame->linesize, outlink->format, outlink->h,
                        ncol, 1);
    } else { // TILT_BLACK and TILT_NONE
        for ( ; ncol < outlink->w; ncol++)
            copy_column(dst->data, dst->linesize,
                        (const uint8_t **)s->black_buffers, s->black_linesizes,
                        outlink->format, outlink->h, ncol, 0);
    }

    // set correct timestamps and props as long as there is proper input
    ret = av_frame_copy_props(dst, s->input->frame);
    if (ret < 0)
        return ret;

    // discard frame at the top of the list since it has been fully processed
    list_remove_head(s);
    // and it is safe to reduce the hold value (even if unused)
    s->hold--;

    // output
    return ff_filter_frame(outlink, dst);
}

#define OFFSET(x) offsetof(TiltandshiftContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "tilt", "Tilt the video horizontally while shifting", OFFSET(tilt), AV_OPT_TYPE_INT,
        { .i64 = 1 }, 0, 1, .flags = V, .unit = "tilt" },

    { "start", "Action at the start of input", OFFSET(start), AV_OPT_TYPE_INT,
        { .i64 = TILT_NONE }, -1, 1, .flags = V, .unit = "start" },
    { "none", "Start immediately (default)", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_NONE }, INT_MIN, INT_MAX, .flags = V, .unit = "start" },
    { "frame", "Use the first frames", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_FRAME }, INT_MIN, INT_MAX, .flags = V, .unit = "start" },
    { "black", "Fill with black", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_BLACK }, INT_MIN, INT_MAX, .flags = V, .unit = "start" },

    { "end", "Action at the end of input", OFFSET(end), AV_OPT_TYPE_INT,
        { .i64 = TILT_NONE }, -1, 1, .flags = V, .unit = "end" },
    { "none", "Do not pad at the end (default)", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_NONE }, INT_MIN, INT_MAX, .flags = V, .unit = "end" },
    { "frame", "Use the last frame", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_FRAME }, INT_MIN, INT_MAX, .flags = V, .unit = "end" },
    { "black", "Fill with black", 0, AV_OPT_TYPE_CONST,
        { .i64 = TILT_BLACK }, INT_MIN, INT_MAX, .flags = V, .unit = "end" },

    { "hold", "Number of columns to hold at the beginning", OFFSET(hold), AV_OPT_TYPE_INT,
        { .i64 = 0 }, 0, INT_MAX, .flags = V, .unit = "hold" },
    { "pad", "Number of columns to pad at the end", OFFSET(pad), AV_OPT_TYPE_INT,
        { .i64 = 0 }, 0, INT_MAX, .flags = V, .unit = "pad" },

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
        .config_props  = config_props,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_tiltandshift = {
    .name          = "tiltandshift",
    .description   = NULL_IF_CONFIG_SMALL("Generate a tilt-and-shift'd video."),
    .priv_size     = sizeof(TiltandshiftContext),
    .priv_class    = &tiltandshift_class,
    .inputs        = tiltandshift_inputs,
    .outputs       = tiltandshift_outputs,
    .query_formats = query_formats,
    .uninit        = uninit,
};
