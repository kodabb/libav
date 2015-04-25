/*
 * VidVox HAP decoder
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
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
 * HAP decoder
 *
 * Fourcc: HAP1, HAP5, HAPY
 *
 * https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
 */

#include <stdint.h>
#include "snappy-c.h"

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "dxtc.h"
#include "hap.h"
#include "internal.h"
#include "thread.h"

/* Texture blocks are 4x4 */
#define BLOCK_W 4
#define BLOCK_H 4

/* The first three bytes are the size of the section past the header, or zero
 * if the length is stored in the next long word. The fourth byte in the first
 * long word indicates the type of the current section. */
static int parse_section_header(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    int length;

    if (bytestream2_get_bytes_left(gbc) < 8)
        return AVERROR_INVALIDDATA;

    length = bytestream2_get_le24(gbc);

    ctx->section_type = bytestream2_get_byte(gbc);

    if (length == 0)
        length = bytestream2_get_le32(gbc);

    if (length > bytestream2_get_bytes_left(gbc) || length == 0)
        return AVERROR_INVALIDDATA;

    return length;
}

/* Prepare the texture to be decompressed */
static int setup_texture(AVCodecContext *avctx, size_t length)
{
    HAPContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    size_t snappy_size;
    const char *texture_name;
    const char *compressor_name;
    int ret;

    switch (ctx->section_type & 0x0F) {
    case FMT_RGBDXT1:
        ctx->tex_rat = 8;
        ctx->tex_fun = ctx->dxtc.dxt1_block;
        texture_name = "DXT1";
        break;
    case FMT_RGBADXT5:
        ctx->tex_rat = 16;
        ctx->tex_fun = ctx->dxtc.dxt5_block;
        texture_name = "DXT5";
        break;
    case FMT_YCOCGDXT5:
        ctx->tex_rat = 16;
        ctx->tex_fun = ctx->dxtc.dxt5ys_block;
        texture_name = "DXT5-YCoCg-scaled";
        break;
    default:
        av_log(avctx, AV_LOG_ERROR,
               "Invalid format mode %02X.\n", ctx->section_type);
        return AVERROR_INVALIDDATA;
    }

    switch (ctx->section_type & 0xF0) {
    case COMP_NONE:
        /* Only texture compression */
        ctx->tex_data = gbc->buffer;
        ctx->tex_size = length;
        compressor_name = "none";
        break;
    case COMP_SNAPPY:
        /* Get the size of the output buffer */
        ret = snappy_uncompressed_length(gbc->buffer, length, &snappy_size);
        if (ret != SNAPPY_OK) {
            av_log(avctx, AV_LOG_ERROR, "Snappy size error\n");
            return AVERROR_BUG;
        }

        /* Resize as needed */
        ret = av_reallocp(&ctx->snappied, snappy_size);
        if (ret < 0)
            return ret;

        /* Uncompress */
        ret = snappy_uncompress(gbc->buffer, length,
                                ctx->snappied, &snappy_size);
        if (ret != SNAPPY_OK) {
            av_log(avctx, AV_LOG_ERROR, "Snappy uncompress error\n");
            return AVERROR_BUG;
        }

        /* Set the pointers */
        ctx->tex_data = ctx->snappied;
        ctx->tex_size = snappy_size;
        compressor_name = "snappy";
        break;
    case COMP_COMPLEX:
        compressor_name = "complex";
        avpriv_request_sample(avctx, "Complex HAP compressor");
        return AVERROR_PATCHWELCOME;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR,
               "Invalid compressor mode %02X.\n", ctx->section_type);
        return AVERROR_INVALIDDATA;
    }

    av_log(avctx, AV_LOG_DEBUG, "%s texture with %s compressor\n",
           texture_name, compressor_name);

    return 0;
}

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int block_nb, int thread_nb)
{
    HAPContext *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    int x = (BLOCK_W * block_nb) % avctx->coded_width;
    int y = BLOCK_H * (BLOCK_W * block_nb / avctx->coded_width);
    uint8_t *p = frame->data[0] + x * PIXEL_SIZE + y * frame->linesize[0];
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_rat;

    ctx->tex_fun(p, frame->linesize[0], d);
    return 0;
}

static int hap_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    HAPContext *ctx = avctx->priv_data;
    ThreadFrame tframe;
    int ret, length;
    int blocks = avctx->coded_width * avctx->coded_height / (BLOCK_W * BLOCK_H);

    bytestream2_init(&ctx->gbc, avpkt->data, avpkt->size);

    /* Check for section header */
    length = parse_section_header(avctx);
    if (length < 0) {
        av_log(avctx, AV_LOG_ERROR, "Frame is too small.\n");
        return length;
    }

    /* Prepare the texture buffer and decompress function */
    ret = setup_texture(avctx, length);
    if (ret < 0)
        return ret;

    /* Get the output frame ready to receive data */
    tframe.f = data;
    ret = ff_thread_get_buffer(avctx, &tframe, 0);
    if (ret < 0)
        return ret;
    ff_thread_finish_setup(avctx);

    /* Use the decompress function on the texture, one block per thread */
    avctx->execute2(avctx, decompress_texture_thread, tframe.f, NULL, blocks);

    /* Frame is ready to be output */
    tframe.f->pict_type = AV_PICTURE_TYPE_I;
    tframe.f->key_frame = 1;
    *got_frame = 1;

    return avpkt->size;
}

static av_cold int hap_init(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4 */
    avctx->coded_width  = FFALIGN(avctx->width,  BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, BLOCK_H);

    /* Technically only one mode has alpha, but 32 bits are easier to handle */
    avctx->pix_fmt = AV_PIX_FMT_RGBA;

    ff_dxtc_decompression_init(&ctx->dxtc);

    return 0;
}

static av_cold int hap_close(AVCodecContext *avctx)
{
    HAPContext *ctx = avctx->priv_data;

    av_freep(&ctx->snappied);

    return 0;
}

AVCodec ff_hap_decoder = {
    .name           = "hap",
    .long_name      = NULL_IF_CONFIG_SMALL("VidVox HAP decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HAP,
    .init           = hap_init,
    .decode         = hap_decode,
    .close          = hap_close,
    .priv_data_size = sizeof(HAPContext),
    .capabilities   = CODEC_CAP_FRAME_THREADS | CODEC_CAP_SLICE_THREADS |
                      CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
