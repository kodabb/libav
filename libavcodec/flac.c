/*
 * FLAC common code
 * Copyright (c) 2009 Justin Ruggles
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

#include "libavutil/channel_layout.h"
#include "libavutil/crc.h"
#include "libavutil/log.h"

#include "bitstream.h"
#include "bytestream.h"
#include "flac.h"
#include "flacdata.h"

static const int8_t sample_size_table[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

static const AVChannelLayout flac_channel_layouts[8] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    AV_CHANNEL_LAYOUT_SURROUND,
    AV_CHANNEL_LAYOUT_QUAD,
    AV_CHANNEL_LAYOUT_5POINT0,
    AV_CHANNEL_LAYOUT_5POINT1,
    AV_CHANNEL_LAYOUT_6POINT1,
    AV_CHANNEL_LAYOUT_7POINT1
};

static int64_t get_utf8(BitstreamContext *bc)
{
    int64_t val;
    GET_UTF8(val, bitstream_read(bc, 8), return -1;)
    return val;
}

int ff_flac_decode_frame_header(AVCodecContext *avctx, BitstreamContext *bc,
                                FLACFrameInfo *fi, int log_level_offset)
{
    int bs_code, sr_code, bps_code;

    /* frame sync code */
    if ((bitstream_read(bc, 15) & 0x7FFF) != 0x7FFC) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset, "invalid sync code\n");
        return AVERROR_INVALIDDATA;
    }

    /* variable block size stream code */
    fi->is_var_size = bitstream_read_bit(bc);

    /* block size and sample rate codes */
    bs_code = bitstream_read(bc, 4);
    sr_code = bitstream_read(bc, 4);

    /* channels and decorrelation */
    fi->ch_mode = bitstream_read(bc, 4);
    if (fi->ch_mode < FLAC_MAX_CHANNELS) {
        fi->channels = fi->ch_mode + 1;
        fi->ch_mode = FLAC_CHMODE_INDEPENDENT;
    } else if (fi->ch_mode < FLAC_MAX_CHANNELS + FLAC_CHMODE_MID_SIDE) {
        fi->channels = 2;
        fi->ch_mode -= FLAC_MAX_CHANNELS - 1;
    } else {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "invalid channel mode: %d\n", fi->ch_mode);
        return AVERROR_INVALIDDATA;
    }

    /* bits per sample */
    bps_code = bitstream_read(bc, 3);
    if (bps_code == 3 || bps_code == 7) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "invalid sample size code (%d)\n",
               bps_code);
        return AVERROR_INVALIDDATA;
    }
    fi->bps = sample_size_table[bps_code];

    /* reserved bit */
    if (bitstream_read_bit(bc)) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "broken stream, invalid padding\n");
        return AVERROR_INVALIDDATA;
    }

    /* sample or frame count */
    fi->frame_or_sample_num = get_utf8(bc);
    if (fi->frame_or_sample_num < 0) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "sample/frame number invalid; utf8 fscked\n");
        return AVERROR_INVALIDDATA;
    }

    /* blocksize */
    if (bs_code == 0) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "reserved blocksize code: 0\n");
        return AVERROR_INVALIDDATA;
    } else if (bs_code == 6) {
        fi->blocksize = bitstream_read(bc, 8) + 1;
    } else if (bs_code == 7) {
        fi->blocksize = bitstream_read(bc, 16) + 1;
    } else {
        fi->blocksize = ff_flac_blocksize_table[bs_code];
    }

    /* sample rate */
    if (sr_code < 12) {
        fi->samplerate = ff_flac_sample_rate_table[sr_code];
    } else if (sr_code == 12) {
        fi->samplerate = bitstream_read(bc, 8) * 1000;
    } else if (sr_code == 13) {
        fi->samplerate = bitstream_read(bc, 16);
    } else if (sr_code == 14) {
        fi->samplerate = bitstream_read(bc, 16) * 10;
    } else {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "illegal sample rate code %d\n",
               sr_code);
        return AVERROR_INVALIDDATA;
    }

    /* header CRC-8 check */
    bitstream_skip(bc, 8);
    if (av_crc(av_crc_get_table(AV_CRC_8_ATM), 0, bc->buffer,
               bitstream_tell(bc) / 8)) {
        av_log(avctx, AV_LOG_ERROR + log_level_offset,
               "header crc mismatch\n");
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

int ff_flac_get_max_frame_size(int blocksize, int ch, int bps)
{
    /* Technically, there is no limit to FLAC frame size, but an encoder
       should not write a frame that is larger than if verbatim encoding mode
       were to be used. */

    int count;

    count = 16;                  /* frame header */
    count += ch * ((7+bps+7)/8); /* subframe headers */
    if (ch == 2) {
        /* for stereo, need to account for using decorrelation */
        count += (( 2*bps+1) * blocksize + 7) / 8;
    } else {
        count += ( ch*bps    * blocksize + 7) / 8;
    }
    count += 2; /* frame footer */

    return count;
}

int ff_flac_is_extradata_valid(AVCodecContext *avctx,
                               enum FLACExtradataFormat *format,
                               uint8_t **streaminfo_start)
{
    if (!avctx->extradata || avctx->extradata_size < FLAC_STREAMINFO_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "extradata NULL or too small.\n");
        return 0;
    }
    if (AV_RL32(avctx->extradata) != MKTAG('f','L','a','C')) {
        /* extradata contains STREAMINFO only */
        if (avctx->extradata_size != FLAC_STREAMINFO_SIZE) {
            av_log(avctx, AV_LOG_WARNING, "extradata contains %d bytes too many.\n",
                   FLAC_STREAMINFO_SIZE-avctx->extradata_size);
        }
        *format = FLAC_EXTRADATA_FORMAT_STREAMINFO;
        *streaminfo_start = avctx->extradata;
    } else {
        if (avctx->extradata_size < 8+FLAC_STREAMINFO_SIZE) {
            av_log(avctx, AV_LOG_ERROR, "extradata too small.\n");
            return 0;
        }
        *format = FLAC_EXTRADATA_FORMAT_FULL_HEADER;
        *streaminfo_start = &avctx->extradata[8];
    }
    return 1;
}

void ff_flac_set_channel_layout(AVCodecContext *avctx, int channels)
{
    if (channels == avctx->ch_layout.nb_channels)
        return;

    av_channel_layout_uninit(&avctx->ch_layout);
    if (channels <= FF_ARRAY_ELEMS(flac_channel_layouts))
        avctx->ch_layout = flac_channel_layouts[channels - 1];
    else
        avctx->ch_layout = (AVChannelLayout){ .order = AV_CHANNEL_ORDER_UNSPEC,
                                              .nb_channels = channels };
}

void ff_flac_parse_streaminfo(AVCodecContext *avctx, struct FLACStreaminfo *s,
                              const uint8_t *buffer)
{
    BitstreamContext bc;
    bitstream_init8(&bc, buffer, FLAC_STREAMINFO_SIZE);

    bitstream_skip(&bc, 16); /* skip min blocksize */
    s->max_blocksize = bitstream_read(&bc, 16);
    if (s->max_blocksize < FLAC_MIN_BLOCKSIZE) {
        av_log(avctx, AV_LOG_WARNING, "invalid max blocksize: %d\n",
               s->max_blocksize);
        s->max_blocksize = 16;
    }

    bitstream_skip(&bc, 24); /* skip min frame size */
    s->max_framesize = bitstream_read(&bc, 24);
    s->samplerate    = bitstream_read(&bc, 20);
    s->channels      = bitstream_read(&bc, 3) + 1;
    s->bps           = bitstream_read(&bc, 5) + 1;

    avctx->sample_rate = s->samplerate;
    avctx->bits_per_raw_sample = s->bps;
    ff_flac_set_channel_layout(avctx, s->channels);

    s->samples  = bitstream_read(&bc, 32) << 4;
    s->samples |= bitstream_read(&bc, 4);

    bitstream_skip(&bc, 64); /* md5 sum */
    bitstream_skip(&bc, 64); /* md5 sum */
}

#if LIBAVCODEC_VERSION_MAJOR < 57
void avpriv_flac_parse_streaminfo(AVCodecContext *avctx, struct FLACStreaminfo *s,
                              const uint8_t *buffer)
{
    ff_flac_parse_streaminfo(avctx, s, buffer);
}

int avpriv_flac_is_extradata_valid(AVCodecContext *avctx,
                               enum FLACExtradataFormat *format,
                               uint8_t **streaminfo_start)
{
    return ff_flac_is_extradata_valid(avctx, format, streaminfo_start);
}
#endif
