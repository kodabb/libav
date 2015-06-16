/*
 * Resolume DXV decoder
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

#include <stdint.h>

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"
#include "texturedsp.h"
#include "thread.h"

typedef struct DXVContext {
    TextureDSPContext texdsp;
    GetByteContext gbc;

    int compressed;
    int paletted;

    uint8_t *tex_data;  // Compressed texture
    int tex_rat;        // Compression ratio
    int tex_size;       // Texture size

    /* Pointer to the selected decompression function */
    int (*tex_funct)(uint8_t *dst, ptrdiff_t stride, const uint8_t *block);
} DXVContext;

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int block_nb, int thread_nb)
{
    DXVContext *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    int x = (TEXTURE_BLOCK_W * block_nb) % avctx->coded_width;
    int y = TEXTURE_BLOCK_H * (TEXTURE_BLOCK_W * block_nb / avctx->coded_width);
    uint8_t *p = frame->data[0] + x * 4 + y * frame->linesize[0];
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_rat;

    ctx->tex_funct(p, frame->linesize[0], d);
    return 0;
}

static void decompress_texture(AVCodecContext *avctx, AVFrame *frame)
{
    DXVContext *ctx = avctx->priv_data;
    int x, y;
    uint8_t *src = ctx->tex_data;

    for (y = 0; y < avctx->height; y += 4) {
        for (x = 0; x < avctx->width; x += 4) {
            uint8_t *p = frame->data[0] + x * 4 + y * frame->linesize[0];
            int step = ctx->tex_funct(p, frame->linesize[0], src);
            src += step;
        }
    }
}

/* DXT5 looks the same except that it works with four
32-bit elements at once and can have a long copy (i.e. more than one element
at a time).  */

/* This scheme addresses already decoded elements depending on 2-bit status:
 *   0 -> copy new element
 *   1 -> copy one element from position -2
 *   2 -> copy one element from position -(get_byte() + 2) * 2
 *   3 -> copy one element from position -(get_16le() + 0x102) * 2 */
#define DXT1_CHECKPOINT()                                                     \
    do {                                                                      \
        if (state == 0) {                                                     \
            value = bytestream2_get_le32(gbc);                                \
            state = 16;                                                       \
        }                                                                     \
        op = value & 0x3;                                                     \
        value >>= 2;                                                          \
        state--;                                                              \
        switch (op) {                                                         \
        case 1: /* copy one element from position -2 */                       \
            idx = 2;                                                          \
            break;                                                            \
        case 2: /* copy one element from position -(get_byte() + 2) * 2 */    \
            idx = (bytestream2_get_byte(gbc) + 2) * 2;                        \
            break;                                                            \
        case 3: /* copy one element from position -(get_16le() + 0x102) * 2 */\
            idx = (bytestream2_get_le16(gbc) + 0x102) * 2;                    \
            break;                                                            \
        }                                                                     \
    } while(0)

static int dxv_decompress_dxt1(AVCodecContext *avctx)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    uint32_t value, op;
    int idx, prev, state = 0;
    int pos = 2;

    /* Copy the first two elements */
    AV_WL32(ctx->tex_data, bytestream2_get_le32(gbc));
    AV_WL32(ctx->tex_data + 4, bytestream2_get_le32(gbc));

    /* Process input until the whole texture has been filled */
    while (pos < ctx->tex_size / 4) {
        DXT1_CHECKPOINT();

        /* Copy two elements from a previous offset or from the input buffer */
        if (op) {
            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
        } else {
            DXT1_CHECKPOINT();

            if (op)
                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            else
                prev = bytestream2_get_le32(gbc);
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            DXT1_CHECKPOINT();

            if (op)
                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            else
                prev = bytestream2_get_le32(gbc);
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
        }
    }

    return 0;
}

static int dxv_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    uint32_t version, tag;
    int ret, blocks, size, consumed;
    int (*decompress_tex)(AVCodecContext *avctx);

    bytestream2_init(gbc, avpkt->data, avpkt->size);

    if (bytestream2_get_bytes_left(gbc) < 20) {
        av_log(avctx, AV_LOG_ERROR, "Frame is too small (%d).",
               bytestream2_get_bytes_left(gbc));
        return AVERROR_INVALIDDATA;
    }

    tag = bytestream2_get_le32(gbc);
    switch (tag) {
    case MKBETAG('D', 'X', 'T', '1'):
        ctx->tex_funct = ctx->texdsp.dxt1_block;
        ctx->tex_rat   = 8;
        decompress_tex = dxv_decompress_dxt1;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported tag header (0x%08X).\n", tag);
        return AVERROR_INVALIDDATA;
    }

    version = bytestream2_get_le32(gbc);
    if (version != 4) {
        av_log(avctx, AV_LOG_WARNING, "version %d\n", version);
    }
    size = bytestream2_get_le32(gbc);
    if (size > bytestream2_get_bytes_left(gbc)) {
        av_log(avctx, AV_LOG_ERROR, "Incomplete file (%u > %u)\n.",
               size, bytestream2_get_bytes_left(gbc));
        return AVERROR_INVALIDDATA;
    }

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    ctx->tex_size = avctx->coded_width * avctx->coded_height * 4 / ctx->tex_rat;
    ret = av_reallocp(&ctx->tex_data, ctx->tex_size);
    if (ret < 0)
        return ret;

    decompress_tex(avctx);

#if 0
    /* Use the decompress function on the texture, one block per thread. */
    blocks = avctx->coded_width * avctx->coded_height / (TEXTURE_BLOCK_W * TEXTURE_BLOCK_H);
    avctx->execute2(avctx, decompress_texture_thread, frame, NULL, blocks);
#else
    decompress_texture(avctx, frame);
#endif

    /* Frame is ready to be output. */
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;
    *got_frame = 1;

    return avpkt->size;
}

static int dxv_init(AVCodecContext *avctx)
{
    DXVContext *ctx = avctx->priv_data;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4. */
    avctx->coded_width  = FFALIGN(avctx->width,  TEXTURE_BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, TEXTURE_BLOCK_H);

    ff_texturedsp_init(&ctx->texdsp);
    avctx->pix_fmt = AV_PIX_FMT_RGBA;

    return 0;
}

static int dxv_close(AVCodecContext *avctx)
{
    DXVContext *ctx = avctx->priv_data;

    av_freep(&ctx->tex_data);

    return 0;
}

AVCodec ff_dxv_decoder = {
    .name           = "dxv",
    .long_name      = NULL_IF_CONFIG_SMALL("Resolume DXV"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DXV,
    .init           = dxv_init,
    .decode         = dxv_decode,
    .close          = dxv_close,
    .priv_data_size = sizeof(DXVContext),
    .capabilities   = CODEC_CAP_DR1,// | CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP
};
