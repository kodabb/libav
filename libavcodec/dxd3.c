/*
 * Resolume DXD3 decoder
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
 * DDS decoder
 *
 * https://msdn.microsoft.com/en-us/library/bb943982%28v=vs.85%29.aspx
 */

#include <stdint.h>

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "texturedsp.h"
#include "thread.h"

#define DDPF_FOURCC    (1 <<  2)
#define DDPF_PALETTE   (1 <<  5)
#define DDPF_NORMALMAP (1 << 31)

typedef struct DXD3Context {
    TextureDSPContext texdsp;
    GetByteContext gbc;

    int compressed;
    int paletted;

    uint8_t *tex_data; // Compressed texture
    int tex_ratio;           // Compression ratio

    /* Pointer to the selected compress or decompress function. */
    int (*tex_funct)(uint8_t *dst, ptrdiff_t stride, const uint8_t *block);
} DXD3Context;

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int block_nb, int thread_nb)
{
    DXD3Context *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    int x = (TEXTURE_BLOCK_W * block_nb) % avctx->coded_width;
    int y = TEXTURE_BLOCK_H * (TEXTURE_BLOCK_W * block_nb / avctx->coded_width);
    uint8_t *p = frame->data[0] + x * 4 + y * frame->linesize[0];
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_ratio;

    ctx->tex_funct(p, frame->linesize[0], d);
    return 0;
}

static const uint16_t DXTR_DistMask[] = {
    0xFF, 0, 0xFF, 0xFF
};

static const int DXTR_DistCost[] = {
      1,   0,   0,   0,   2,   0,   0,   0,   0, 0,  0, 0, 0, 0, 0,  0,  0,  0,
      1,   0,   2,   0,   3,   0,   4,   0,   5, 0,  6, 0, 7, 0, 8,  0,  9,  0,
     10,   0,  11,   0,  12,   0,  13,   0,  14, 0, 15, 0, 4, 0, 0,  0,  5,  0,
      0,   0,   6,   0,   0,   0,   7,   0,   0, 0,  0, 2, 4, 6, 8, 10, 12, 14,
    128, 128, 128, 128, 128, 128, 128, 128,
};

static const int DXTR_DistOffset[] = {
    1, 0, 0, 0, 2, 0, 0, 0, 2, 1, 0, 0
};

static int DXTR_uncompressDXT1(uint32_t *dst, const uint8_t *inbuf, size_t size)
{
    const uint8_t *src_;
    uint32_t *out;
    unsigned int state;
    unsigned int v7;
    int v8;
    unsigned int v9;
    uint16_t v10;
    int v11;
    uint32_t *v12;
    int v13;
    int v14;
    const uint8_t *src;
    uint16_t v16;
    int v17;
    uint16_t v18;

    src_ = inbuf + 8;
    *dst = *(uint32_t *)inbuf;
    dst[1] = *((uint32_t *)inbuf + 1);
    if (size >= 9) {
        out = dst + 2;
        state = 2;
        while (1) {
            if (state == 2) {
                state = *(uint32_t *)src_;
                src_ += 4;
                v7 = (state >> 2) | 0x80000000;
            } else {
                v7 = state >> 2;
            }
            v8 = state & 3;
            if (v8)
                break;

            if (v7 == 2) {
                v7 = *(uint32_t *)src_;
                src_ += 4;
                v9 = (v7 >> 2) | 0x80000000;
            } else {
                v9 = v7 >> 2;
            }

            v14 = v7 & 3;
            if (v14) {
                v16 = *(uint16_t *)src_ & DXTR_DistMask[v14];
                src = &src_[DXTR_DistCost[v14]];
                *out = out[-2 * (DXTR_DistOffset[v14] + v16)];
            } else {
                *out = *(uint32_t *)src_;
                src = src_ + 4;
            }

            if (v9 == 2) {
                v9 = *(uint32_t *)src;
                src += 4;
                v7 = (v9 >> 2) | 0x80000000;
            } else {
                v7 = v9 >> 2;
            }

            v17 = v9 & 3;
            if (v17) {
                v18 = *(uint16_t *)src & DXTR_DistMask[v17];
                src_ = &src[DXTR_DistCost[v17]];
                v13 = -8 * (DXTR_DistOffset[v17] + v18);
                goto LABEL_20;
            }
            out[1] = *(uint32_t *)src;
            src_ = src + 4;
LABEL_21:
            out += 2;
            state = v7;
            if (out >= (uint32_t *)((char *)dst + size))
                return src_ - inbuf;
        }

        v10 = *(uint16_t *)src_ & DXTR_DistMask[v8];
        src_ += DXTR_DistCost[v8];
        v11 = 8 * (DXTR_DistOffset[v8] + v10);
        v12 = &out[-v11 / 4u];
        v13 = -v11;
        *out = *v12;
LABEL_20:
        out[1] = *(uint32_t *)((char *)out + v13 + 4);
        goto LABEL_21;
    }

    return src_ - inbuf;
}

static int dxd3_decode(AVCodecContext *avctx, void *data,
                       int *got_frame, AVPacket *avpkt)
{
    DXD3Context *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    uint32_t version, tag;
    int ret, blocks, size;

    bytestream2_init(gbc, avpkt->data, avpkt->size);

    if (bytestream2_get_bytes_left(gbc) < 20) {
        av_log(avctx, AV_LOG_ERROR, "Frame is too small (%d).",
               bytestream2_get_bytes_left(gbc));
        return AVERROR_INVALIDDATA;
    }

    tag = bytestream2_get_le32(gbc);
    if (tag != MKBETAG('D', 'X', 'T', '1')) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported tag header (0x%08X).\n", tag);
        return AVERROR_INVALIDDATA;
    }
    ctx->tex_funct = ctx->texdsp.dxt1_block;

    version = bytestream2_get_le32(gbc);
    if (version != 4) {
        av_log(avctx, AV_LOG_WARNING, "version %d\n", version);
    }
    size = bytestream2_get_le32(gbc);
    if (size != bytestream2_get_bytes_left(gbc)) {
        av_log(avctx, AV_LOG_ERROR, "Incomplete file (%u > %u)\n.",
               size, bytestream2_get_bytes_left(gbc));
    }

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    /* Use the decompress function on the texture, one block per thread. */
    ret = av_reallocp(&ctx->tex_data, 1920*1080*4/8);
    if (ret < 0)
        return ret;

    DXTR_uncompressDXT1((uint32_t *)ctx->tex_data, gbc->buffer, 1920*1080*4/8);
    blocks = avctx->coded_width * avctx->coded_height / (TEXTURE_BLOCK_W * TEXTURE_BLOCK_H);
    avctx->execute2(avctx, decompress_texture_thread, frame, NULL, blocks);

    /* Frame is ready to be output. */
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;
    *got_frame = 1;

    return avpkt->size;
}

static int dxd3_init(AVCodecContext *avctx)
{
    DXD3Context *ctx = avctx->priv_data;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4. */
    avctx->coded_width  = FFALIGN(avctx->width,  TEXTURE_BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, TEXTURE_BLOCK_H);

    avctx->pix_fmt = AV_PIX_FMT_RGBA;

    ff_texturedsp_init(&ctx->texdsp);

    return 0;
}

static int dxd3_close(AVCodecContext *avctx)
{
    DXD3Context *ctx = avctx->priv_data;

    av_freep(&ctx->tex_data);

    return 0;
}

AVCodec ff_dxd3_decoder = {
    .name           = "dxd3",
    .long_name      = NULL_IF_CONFIG_SMALL("Resolume DXD3"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DXD3,
    .init           = dxd3_init,
    .decode         = dxd3_decode,
    .close          = dxd3_close,
    .priv_data_size = sizeof(DXD3Context),
    .capabilities   = CODEC_CAP_DR1 | CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP
};
