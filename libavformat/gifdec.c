/*
 * GIF demuxer
 * Copyright (c) 2012 Vitaliy E Sugrobov
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
 * GIF demuxer.
 *
 * There are two options available to user: default_delay and min_delay.
 *
 * These options are for protection from too rapid gif animations.
 * In practice it is standard approach to slow down rendering of this
 * kind of gifs. If you try to play gif with delay between frames of one
 * hundredth of second (100fps) using one of major web browsers, you get
 * significantly slower playback, around 10 fps. This is because browser
 * detects that delay value is less than some threshold (usually 2 hundredths
 * of second) and reset it to default value (usually 10 hundredths of second,
 * which corresponds to 10fps). Manipulating these options, user can achieve
 * the same effect during conversion to some video format. Otherwise user
 * can set them to not protect from rapid animations at all.
 *
 * The other case when these options necessary is for gif images encoded
 * according to gif87a standard since prior to gif89a there was no delay
 * information included in file.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "internal.h"

#define GIF_SIG_87A "GIF87a"
#define GIF_SIG_89A "GIF89a"

#define GIF_TRAILER                 0x3b
#define GIF_EXTENSION_INTRODUCER    0x21
#define GIF_IMAGE_SEPARATOR         0x2c
#define GIF_GCE_EXT_LABEL           0xf9

typedef struct GIFDemuxContext {
    const AVClass *class;
    uint32_t width;
    uint32_t height;

    /**
     * Time span in hundredths of second before
     * the next frame should be drawn on screen.
     */
    int delay;

    /**
     * Minimum allowed delay between frames in hundredths of
     * second. Values below this threshold considered to be
     * invalid and set to value of default_delay.
     */
    int min_delay;
    int default_delay;
    int total_duration; ///< In hundredths of second.
} GIFDemuxContext;

/**
 * Major web browsers display gifs at ~10-15fps when rate is not explicitly
 * set or have too low values. We assume default rate to be 10.
 * Default delay = 100hundredths of second / 10fps = 10hos per frame.
 */
#define GIF_DEFAULT_DELAY   10

/**
 * By default delay values less than this threshold considered to be invalid.
 */
#define GIF_MIN_DELAY       2

static int gif_probe(AVProbeData *p)
{
    /* check magick */
    if (memcmp(p->buf, GIF_SIG_87A, 6) && memcmp(p->buf, GIF_SIG_89A, 6))
        return 0;

    /* width or height contains zero? */
    if (!AV_RL16(&p->buf[6]) || !AV_RL16(&p->buf[8]))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int gif_read_header(AVFormatContext *s)
{
    GIFDemuxContext *gdc = s->priv_data;
    AVIOContext     *pb  = s->pb;
    AVStream        *st;
    int ret;

    /* skip 6-byte magick */
    if ((ret = avio_skip(pb, 6)) < 0)
        return ret;

    gdc->delay  = gdc->default_delay;
    gdc->width  = avio_rl16(pb);
    gdc->height = avio_rl16(pb);

    if (gdc->width == 0 || gdc->height == 0)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    /* GIF format operates with time in "hundredths of second",
     * therefore timebase is 1/100 */
    avpriv_set_pts_info(st, 64, 1, 100);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_GIF;
    st->codecpar->width      = gdc->width;
    st->codecpar->height     = gdc->height;

    /* jump to start because gif decoder needs header data too */
    return avio_seek(pb, 0, SEEK_SET);
}

static int gif_skip_subblocks(AVIOContext *pb)
{
    int sb_size, ret = 0;

    while ((sb_size = avio_r8(pb)) > 0) {
        if ((ret = avio_skip(pb, sb_size)) < 0)
            return ret;
    }

    return ret;
}

static int gif_read_ext(AVFormatContext *s)
{
    GIFDemuxContext *gdc = s->priv_data;
    AVIOContext *pb = s->pb;
    int sb_size, ext_label = avio_r8(pb);
    int ret;

    if (ext_label != GIF_GCE_EXT_LABEL)
        return gif_skip_subblocks(pb);

    if ((sb_size = avio_r8(pb)) < 4) {
        av_log(s, AV_LOG_FATAL,
               "Graphic Control Extension block's size less than 4.\n");
        return AVERROR_INVALIDDATA;
    }

    /* skip packed fields */
    if ((ret = avio_skip(pb, 1)) < 0)
        return ret;

    gdc->delay = avio_rl16(pb);

    if (gdc->delay < gdc->min_delay)
        gdc->delay = gdc->default_delay;

    /* skip the rest of the Graphic Control Extension block */
    if ((ret = avio_skip(pb, sb_size - 3)) < 0)
        return ret;

    return gif_skip_subblocks(pb);
}

static int gif_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    GIFDemuxContext *gdc = s->priv_data;
    AVIOContext *pb = s->pb;
    int packed_fields, block_label, ct_size;
    int keyframe, frame_parsed = 0;
    int ret;
    int64_t frame_start = avio_tell(pb), frame_end;
    unsigned char buf[6];

    if ((ret = avio_read(pb, buf, 6)) < 0)
        return ret;

    keyframe = memcmp(buf, GIF_SIG_87A, 6) == 0 ||
               memcmp(buf, GIF_SIG_89A, 6) == 0;

    if (keyframe) {
        /* skip 2 bytes of width and 2 of height */
        if ((ret = avio_skip(pb, 4)) < 0)
            return ret;

        packed_fields = avio_r8(pb);

        /* skip 1 byte of Background Color Index and 1 byte of Pixel Aspect Ratio */
        if ((ret = avio_skip(pb, 2)) < 0)
            return ret;

        /* glogal color table presence */
        if (packed_fields & 0x80) {
            ct_size = 3 * (1 << ((packed_fields & 0x07) + 1));

            if ((ret = avio_skip(pb, ct_size)) < 0)
                return ret;
        }

        gdc->total_duration = 0;
    } else {
        avio_seek(pb, -ret, SEEK_CUR);
        ret = AVERROR_EOF;
    }

    while (GIF_TRAILER != (block_label = avio_r8(pb)) && !pb->eof_reached) {
        switch(block_label) {
        case GIF_EXTENSION_INTRODUCER:
            if ((ret = gif_read_ext(s)) < 0 )
                return ret;
            break;
        case GIF_IMAGE_SEPARATOR:
            /* skip to last byte of Image Descriptor header */
            if ((ret = avio_skip(pb, 8)) < 0)
                return ret;

            packed_fields = avio_r8(pb);

            /* local color table presence */
            if (packed_fields & 0x80) {
                ct_size = 3 * (1 << ((packed_fields & 0x07) + 1));

                if ((ret = avio_skip(pb, ct_size)) < 0)
                    return ret;
            }

            /* read LZW Minimum Code Size */
            if (avio_r8(pb) < 1) {
                av_log(s, AV_LOG_ERROR, "lzw minimum code size must be >= 1\n");
                return AVERROR_INVALIDDATA;
            }

            if ((ret = gif_skip_subblocks(pb)) < 0)
                return ret;

            frame_end = avio_tell(pb);

            if (avio_seek(pb, frame_start, SEEK_SET) != frame_start)
                return AVERROR(EIO);

            ret = av_get_packet(pb, pkt, frame_end - frame_start);
            if (ret < 0)
                return ret;

            if (keyframe)
                pkt->flags |= AV_PKT_FLAG_KEY;

            pkt->dts =
            pkt->pts = gdc->total_duration;

            gdc->total_duration += gdc->delay;
            pkt->duration = gdc->delay;
            pkt->stream_index = 0;

            /* Graphic Control Extension's scope is single frame.
             * Remove its influence. */
            gdc->delay = gdc->default_delay;
            frame_parsed = 1;
            goto end;
            break;
        default:
            av_log(s, AV_LOG_ERROR,
                   "invalid block label (0x%02X)\n", block_label);
            return AVERROR_INVALIDDATA;
        }
    }

end:
    if (ret >= 0 && !frame_parsed) {
        /* This might happen when there is no image block
         * between extension blocks and GIF_TRAILER or EOF */
        return AVERROR_EOF;
    }

    return ret;
}

#define OFFSET(x) offsetof(GIFDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "min_delay", "minimum valid delay between frames (in hundredths of second)", OFFSET(min_delay), AV_OPT_TYPE_INT, { .i64 = GIF_MIN_DELAY }, 0, 100 * 60, DEC },
    { "default_delay", "default delay between frames (in hundredths of second)", OFFSET(default_delay), AV_OPT_TYPE_INT, { .i64 = GIF_DEFAULT_DELAY }, 0, 100 * 60, DEC },
    { NULL },
};

static const AVClass demuxer_class = {
    .class_name = "GIF demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_gif_demuxer = {
    .name           = "gif",
    .long_name      = NULL_IF_CONFIG_SMALL("GIF (Graphics Interchange Format)"),
    .priv_data_size = sizeof(GIFDemuxContext),
    .read_probe     = gif_probe,
    .read_header    = gif_read_header,
    .read_packet    = gif_read_packet,
    .priv_class     = &demuxer_class,
};
