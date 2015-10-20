/*
 * daala ogg demuxer
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
 * Copyright (C) 2015 Rostislav Pehlivanov <atomnuker@gmail.com>
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

#include <stdlib.h>
#include "libavutil/bswap.h"
#include "libavcodec/bytestream.h"
#include "avformat.h"
#include "internal.h"
#include "oggdec.h"

typedef struct DaalaParams {
    int gpshift;
    int gpmask;
    int header_nb;
    unsigned int version;
} DaalaParams;

static int daala_header(AVFormatContext *s, int idx)
{
    struct ogg *ogg       = s->priv_data;
    struct ogg_stream *os = ogg->streams + idx;
    AVStream *st          = s->streams[idx];
    DaalaParams *dpar     = os->private;
    int cds               = st->codec->extradata_size + os->psize + 2;
    GetByteContext gb;
    int err;
    uint8_t *cdp;

    if (!(os->buf[os->pstart] & 0x80))
        return 0;

    if (!dpar) {
        dpar = av_mallocz(sizeof(*dpar));
        if (!dpar)
            return AVERROR(ENOMEM);
        os->private = dpar;
    }

    switch (os->buf[os->pstart]) {
    case 0x80: {
        AVRational timebase;
        int nplanes;

        bytestream2_init(&gb, os->buf + os->pstart, os->psize);

        /* 0x80"daala" */
        bytestream2_skip(&gb, 6);

        dpar->version = bytestream2_get_le24(&gb);

        st->codec->width  = bytestream2_get_le32(&gb);
        st->codec->height = bytestream2_get_le32(&gb);

        st->sample_aspect_ratio.num = bytestream2_get_le32(&gb);
        st->sample_aspect_ratio.den = bytestream2_get_le32(&gb);

        timebase.den = bytestream2_get_le32(&gb);
        timebase.num = bytestream2_get_le32(&gb);
        if (timebase.num <= 0 && timebase.den <= 0) {
            av_log(s, AV_LOG_WARNING,
                   "Invalid time base (%d/%d), assuming 25fps\n",
                   timebase.num, timebase.den);
            timebase.num = 1;
            timebase.den = 25;
        }
        avpriv_set_pts_info(st, 64, timebase.num, timebase.den);

        bytestream2_skip(&gb, 4); // frameduration
        dpar->gpshift = bytestream2_get_byte(&gb);
        dpar->gpmask  = (1 << dpar->gpshift) - 1;

        bytestream2_skip(&gb, 1); // bitdepth
        nplanes = bytestream2_get_byte(&gb);
        bytestream2_skip(&gb, 2 * nplanes); // planeinfo

        st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codec->codec_id   = AV_CODEC_ID_DAALA;
    }
    break;
    case 0x81:
        if (dpar->header_nb != 1) {
            av_log(s, AV_LOG_ERROR,
                   "Header 0x%02X received out of order (position %d).\n",
                   os->buf[os->pstart], dpar->header_nb);
            return AVERROR_INVALIDDATA;
        }

        /* 0x81"daala" */
        ff_vorbis_stream_comment(s, st,
                                 os->buf + os->pstart + 6, os->psize - 6);
        break;
    case 0x82:
        if (dpar->header_nb != 2) {
            av_log(s, AV_LOG_ERROR,
                   "Header 0x%02X received out of order (position %d).\n",
                   os->buf[os->pstart], dpar->header_nb);
            return AVERROR_INVALIDDATA;
        }

        /* 0x82"daala" */
    break;
    default:
        av_log(s, AV_LOG_ERROR, "Unknown header %02X\n", os->buf[os->pstart]);
        return AVERROR_INVALIDDATA;
    }

    if ((err = av_reallocp(&st->codec->extradata,
                           cds + AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
        st->codec->extradata_size = 0;
        return err;
    }

    cdp    = st->codec->extradata + st->codec->extradata_size;
    *cdp++ = os->psize >> 8;
    *cdp++ = os->psize & 0xff;
    memcpy(cdp, os->buf + os->pstart, os->psize);
    st->codec->extradata_size = cds;

    dpar->header_nb++;
    return 1;
}

static uint64_t daala_gptopts(AVFormatContext *ctx, int idx,
                              uint64_t gp, int64_t *dts)
{
    struct ogg *ogg       = ctx->priv_data;
    struct ogg_stream *os = ogg->streams + idx;
    DaalaParams *dpar     = os->private;
    uint64_t iframe, pframe;

    if (!dpar)
        return AV_NOPTS_VALUE;

    iframe = gp >> dpar->gpshift;
    pframe = gp & dpar->gpmask;

    if (!pframe)
        os->pflags |= AV_PKT_FLAG_KEY;

    if (dts)
        *dts = iframe + pframe;

    return iframe + pframe;
}

static int daala_packet(AVFormatContext *s, int idx)
{
    struct ogg *ogg = s->priv_data;
    struct ogg_stream *os = ogg->streams + idx;
    int duration;

    /* first packet handling
     * here we parse the duration of each packet in the first page and compare
     * the total duration to the page granule to find the encoder delay and
     * set the first timestamp */
    if ((!os->lastpts || os->lastpts == AV_NOPTS_VALUE) &&
        !(os->flags & OGG_FLAG_EOS)) {
        int seg;

        duration = 1;
        for (seg = os->segp; seg < os->nsegs; seg++) {
            if (os->segments[seg] < 255)
                duration++;
        }

        os->lastpts =
        os->lastdts = daala_gptopts(s, idx, os->granule, NULL) - duration;
        if (s->streams[idx]->start_time == AV_NOPTS_VALUE) {
            s->streams[idx]->start_time = os->lastpts;
            if (s->streams[idx]->duration)
                s->streams[idx]->duration -= s->streams[idx]->start_time;
        }
    }

    /* parse packet duration */
    if (os->psize > 0) {
        os->pduration = 1;
    }

    return 0;
}

const struct ogg_codec ff_daala_codec = {
    .name             = "Daala",
    .magic            = "\200daala",
    .magicsize        = 6,
    .header           = daala_header,
    .packet           = daala_packet,
    .gptopts          = daala_gptopts,
    .granule_is_start = 1,
    .nb_header        = 3,
};
