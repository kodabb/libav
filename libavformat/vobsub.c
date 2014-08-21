/*
 * MPEG1/2 demuxer
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avformat.h"
#include "internal.h"
#include "mpeg.h"
#include "../../ffmpeg/libavformat/subtitles.h"
//#include "libavutil/bprint.h"
#include "libavutil/mathematics.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"

typedef struct VobSubContext {
    AVFormatContext *sub_ctx;
    FFDemuxSubtitlesQueue q[32];
} VobSubContext;

#define REF_STRING "# VobSub index file,"
#define MAX_LINE_SIZE 2048

static int vobsub_probe(AVProbeData *p)
{
    if (!strncmp(p->buf, REF_STRING, sizeof(REF_STRING) - 1))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int vobsub_read_header(AVFormatContext *s)
{
    int i, ret = 0, header_parsed = 0, langidx = 0;
    VobSubContext *vobsub = s->priv_data;
    char *sub_name = NULL;
    size_t fname_len;
    char *ext, *header_str;
    AVBPrint header;
    int64_t delay = 0;
    AVStream *st = NULL;
    int stream_id = -1;
    char id[64] = {0};
    char alt[MAX_LINE_SIZE] = {0};

    sub_name = av_strdup(s->filename);
    fname_len = strlen(sub_name);
    ext = sub_name - 3 + fname_len;
    if (fname_len < 4 || *(ext - 1) != '.') {
        av_log(s, AV_LOG_ERROR, "The input index filename is too short "
               "to guess the associated .SUB file\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    memcpy(ext, !strncmp(ext, "IDX", 3) ? "SUB" : "sub", 3);
    av_log(s, AV_LOG_VERBOSE, "IDX/SUB: %s -> %s\n", s->filename, sub_name);
    ret = avformat_open_input(&vobsub->sub_ctx, sub_name, &ff_mpegps_demuxer, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open %s as MPEG subtitles\n", sub_name);
        goto end;
    }

    av_bprint_init(&header, 0, AV_BPRINT_SIZE_UNLIMITED);
    while (!s->pb->eof_reached) {
        char line[MAX_LINE_SIZE];
        int len = ff_get_line(s->pb, line, sizeof(line));

        if (!len)
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (!strncmp(line, "id:", 3)) {
            if (sscanf(line, "id: %63[^,], index: %u", id, &stream_id) != 2) {
                av_log(s, AV_LOG_WARNING, "Unable to parse index line '%s', "
                       "assuming 'id: und, index: 0'\n", line);
                strcpy(id, "und");
                stream_id = 0;
            }

            if (stream_id >= FF_ARRAY_ELEMS(vobsub->q)) {
                av_log(s, AV_LOG_ERROR, "Maximum number of subtitles streams reached\n");
                ret = AVERROR(EINVAL);
                goto end;
            }

            header_parsed = 1;
            alt[0] = '\0';
            /* We do not create the stream immediately to avoid adding empty
             * streams. See the following timestamp entry. */

            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] id=%s\n", stream_id, id);

        } else if (!strncmp(line, "timestamp:", 10)) {
            AVPacket *sub;
            int hh, mm, ss, ms;
            int64_t pos, timestamp;
            const char *p = line + 10;

            if (stream_id == -1) {
                av_log(s, AV_LOG_ERROR, "Timestamp declared before any stream\n");
                ret = AVERROR_INVALIDDATA;
                goto end;
            }

            if (!st || st->id != stream_id) {
                st = avformat_new_stream(s, NULL);
                if (!st) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
                st->id = stream_id;
                st->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;
                st->codec->codec_id   = AV_CODEC_ID_DVD_SUBTITLE;
                avpriv_set_pts_info(st, 64, 1, 1000);
                av_dict_set(&st->metadata, "language", id, 0);
                if (alt[0])
                    av_dict_set(&st->metadata, "title", alt, 0);
            }

            if (sscanf(p, "%02d:%02d:%02d:%03d, filepos: %"SCNx64,
                       &hh, &mm, &ss, &ms, &pos) != 5) {
                av_log(s, AV_LOG_ERROR, "Unable to parse timestamp line '%s', "
                       "abort parsing\n", line);
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            timestamp = (hh*3600LL + mm*60LL + ss) * 1000LL + ms + delay;
            timestamp = av_rescale_q(timestamp, (AVRational) {1, 1000}, st->time_base);

            sub = ff_subtitles_queue_insert(&vobsub->q[s->nb_streams - 1], "", 0, 0);
            if (!sub) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            sub->pos = pos;
            sub->pts = timestamp;
            sub->stream_index = s->nb_streams - 1;

        } else if (!strncmp(line, "alt:", 4)) {
            const char *p = line + 4;

            while (*p == ' ')
                p++;
            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] name=%s\n", st->id, p);
            av_strlcpy(alt, p, sizeof(alt));
            header_parsed = 1;

        } else if (!strncmp(line, "delay:", 6)) {
            int sign = 1, hh = 0, mm = 0, ss = 0, ms = 0;
            const char *p = line + 6;

            while (*p == ' ')
                p++;
            if (*p == '-' || *p == '+') {
                sign = *p == '-' ? -1 : 1;
                p++;
            }
            sscanf(p, "%d:%d:%d:%d", &hh, &mm, &ss, &ms);
            delay = ((hh*3600LL + mm*60LL + ss) * 1000LL + ms) * sign;

        } else if (!strncmp(line, "langidx:", 8)) {
            const char *p = line + 8;

            if (sscanf(p, "%d", &langidx) != 1)
                av_log(s, AV_LOG_ERROR, "Invalid langidx specified\n");

        } else if (!header_parsed) {
            if (line[0] && line[0] != '#')
                av_bprintf(&header, "%s\n", line);
        }
    }

    if (langidx < s->nb_streams)
        s->streams[langidx]->disposition |= AV_DISPOSITION_DEFAULT;

    for (i = 0; i < s->nb_streams; i++) {
        vobsub->q[i].sort = SUB_SORT_POS_TS;
        ff_subtitles_queue_finalize(&vobsub->q[i]);
    }

    if (!av_bprint_is_complete(&header)) {
        av_bprint_finalize(&header, NULL);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    av_bprint_finalize(&header, &header_str);
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *sub_st = s->streams[i];
        sub_st->codec->extradata      = av_strdup(header_str);
        sub_st->codec->extradata_size = header.len;
    }
    av_free(header_str);

end:
    av_free(sub_name);
    return ret;
}

static int vobsub_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    VobSubContext *vobsub = s->priv_data;
    FFDemuxSubtitlesQueue *q;
    AVIOContext *pb = vobsub->sub_ctx->pb;
    int ret, psize, total_read = 0, i;
    AVPacket idx_pkt;

    int64_t min_ts = INT64_MAX;
    int sid = 0;
    for (i = 0; i < s->nb_streams; i++) {
        FFDemuxSubtitlesQueue *tmpq = &vobsub->q[i];
        int64_t ts;
        av_assert0(tmpq->nb_subs);
        ts = tmpq->subs[tmpq->current_sub_idx].pts;
        if (ts < min_ts) {
            min_ts = ts;
            sid = i;
        }
    }
    q = &vobsub->q[sid];
    ret = ff_subtitles_queue_read_packet(q, &idx_pkt);
    if (ret < 0)
        return ret;

    /* compute maximum packet size using the next packet position. This is
     * useful when the len in the header is non-sense */
    if (q->current_sub_idx < q->nb_subs) {
        psize = q->subs[q->current_sub_idx].pos - idx_pkt.pos;
    } else {
        int64_t fsize = avio_size(pb);
        psize = fsize < 0 ? 0xffff : fsize - idx_pkt.pos;
    }

    avio_seek(pb, idx_pkt.pos, SEEK_SET);

    av_init_packet(pkt);
    pkt->size = 0;
    pkt->data = NULL;

    do {
        int n, to_read, startcode;
        int64_t pts, dts;
        int64_t old_pos = avio_tell(pb), new_pos;
        int pkt_size;

        ret = ff_mpegps_read_pes_header(vobsub->sub_ctx, NULL, &startcode, &pts, &dts);
        if (ret < 0) {
            if (pkt->size) // raise packet even if incomplete
                break;
            goto fail;
        }
        to_read = ret & 0xffff;
        new_pos = avio_tell(pb);
        pkt_size = ret + (new_pos - old_pos);

        /* this prevents reads above the current packet */
        if (total_read + pkt_size > psize)
            break;
        total_read += pkt_size;

        /* the current chunk doesn't match the stream index (unlikely) */
        if ((startcode & 0x1f) != idx_pkt.stream_index)
            break;

        ret = av_grow_packet(pkt, to_read);
        if (ret < 0)
            goto fail;

        n = avio_read(pb, pkt->data + (pkt->size - to_read), to_read);
        if (n < to_read)
            pkt->size -= to_read - n;
    } while (total_read < psize);

    pkt->pts = pkt->dts = idx_pkt.pts;
    pkt->pos = idx_pkt.pos;
    pkt->stream_index = idx_pkt.stream_index;

    av_free_packet(&idx_pkt);
    return 0;

fail:
    av_free_packet(pkt);
    av_free_packet(&idx_pkt);
    return ret;
}

static int vobsub_read_seek(AVFormatContext *s, int stream_index,
                            int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    VobSubContext *vobsub = s->priv_data;

    /* Rescale requested timestamps based on the first stream (timebase is the
     * same for all subtitles stream within a .idx/.sub). Rescaling is done just
     * like in avformat_seek_file(). */
    if (stream_index == -1 && s->nb_streams != 1) {
        int i, ret = 0;
        AVRational time_base = s->streams[0]->time_base;
        ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);
        min_ts = av_rescale_rnd(min_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_UP);//   | AV_ROUND_PASS_MINMAX);
        max_ts = av_rescale_rnd(max_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_DOWN);// | AV_ROUND_PASS_MINMAX);
        for (i = 0; i < s->nb_streams; i++) {
            int r = ff_subtitles_queue_seek(&vobsub->q[i], s, stream_index,
                                            min_ts, ts, max_ts, flags);
            if (r < 0)
                ret = r;
        }
        return ret;
    }

    if (stream_index == -1) // only 1 stream
        stream_index = 0;
    return ff_subtitles_queue_seek(&vobsub->q[stream_index], s, stream_index,
                                   min_ts, ts, max_ts, flags);
}

static int vobsub_read_close(AVFormatContext *s)
{
    int i;
    VobSubContext *vobsub = s->priv_data;

    for (i = 0; i < s->nb_streams; i++)
        ff_subtitles_queue_clean(&vobsub->q[i]);
    if (vobsub->sub_ctx)
        avformat_close_input(&vobsub->sub_ctx);
    return 0;
}

AVInputFormat ff_vobsub_demuxer = {
    .name           = "vobsub",
    .long_name      = NULL_IF_CONFIG_SMALL("VobSub subtitle format"),
    .priv_data_size = sizeof(VobSubContext),
    .read_probe     = vobsub_probe,
    .read_header    = vobsub_read_header,
    .read_packet    = vobsub_read_packet,
    .read_seek2     = vobsub_read_seek,
    .read_close     = vobsub_read_close,
    .flags          = AVFMT_SHOW_IDS,
    .extensions     = "idx",
};
