/*
 * DirectDraw Surface image decoder
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
#include "dxtc.h"
#include "hap.h"
#include "internal.h"
#include "thread.h"

/* Texture blocks are 4x4 */
#define BLOCK_W 4
#define BLOCK_H 4

#define DDPF_FOURCC  (1 << 2)
#define DDPF_PALETTE (1 << 5)

typedef struct DDSContext {
    AVClass *class;

    DXTCContext dxtc;
    GetByteContext gbc;

    int compressed;
    int paletted;
    const uint8_t *tex_data; /* Compressed texture */
    int tex_rat;             /* Compression ratio */

    /* Pointer to the selected compress or decompress function */
    int (*tex_fun)(uint8_t *dst, ptrdiff_t stride, const uint8_t *block);
} DDSContext;

static int parse_pixel_format(AVCodecContext *avctx)
{
    DDSContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    char buf[32];
    uint32_t flags, fourcc;
    int size, bpp, r, g, b, a;

    size = bytestream2_get_le32(gbc);
    if (size != 32) {
        av_log(avctx, AV_LOG_ERROR, "Invalid pixel format header %d.\n", size);
        return AVERROR_INVALIDDATA;
    }
    flags = bytestream2_get_le32(gbc);
    ctx->compressed = flags & DDPF_FOURCC;
    ctx->paletted   = flags & DDPF_PALETTE;
    fourcc = bytestream2_get_le32(gbc);

    bpp = bytestream2_get_le32(gbc); // rgbbitcount
    r   = bytestream2_get_le32(gbc); // rbitmask
    g   = bytestream2_get_le32(gbc); // gbitmask
    b   = bytestream2_get_le32(gbc); // bbitmask
    a   = bytestream2_get_le32(gbc); // abitmask

    av_get_codec_tag_string(buf, sizeof(buf), fourcc);
    av_log(avctx, AV_LOG_VERBOSE, "fourcc %s bpp %d "
           "r 0x%x g 0x%x b 0x%x a 0x%x.\n", buf, bpp, r, g, b, a);

    if (ctx->compressed) {
        switch (fourcc) {
        case MKTAG('D', 'X', 'T', '1'):
            ctx->tex_rat = 8;
            ctx->tex_fun = ctx->dxtc.dxt1a_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '2'):
            ctx->tex_rat = 16;
            ctx->tex_fun = ctx->dxtc.dxt2_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '3'):
            ctx->tex_rat = 16;
            ctx->tex_fun = ctx->dxtc.dxt3_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '4'):
            ctx->tex_rat = 16;
            ctx->tex_fun = ctx->dxtc.dxt4_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '5'):
            ctx->tex_rat = 16;
            ctx->tex_fun = ctx->dxtc.dxt5_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('U', 'Y', 'V', 'Y'):
            ctx->compressed = 0;
            avctx->pix_fmt = AV_PIX_FMT_UYVY422;
            break;
        case MKTAG('Y', 'U', 'Y', '2'):
            ctx->compressed = 0;
            avctx->pix_fmt = AV_PIX_FMT_YUYV422;
            break;
        case MKTAG('A', 'T', 'I', '1'):
        case MKTAG('A', 'T', 'I', '2'):
        case MKTAG('D', 'X', '1', '0'):
            avpriv_report_missing_feature(avctx, "Texture type %s", buf);
            return AVERROR_PATCHWELCOME;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported %s fourcc.\n", buf);
            return AVERROR_INVALIDDATA;
        }
    } else if (ctx->paletted) {
        if (bpp == 8) {
            avctx->pix_fmt = AV_PIX_FMT_PAL8;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported palette bpp %d.\n", bpp);
            return AVERROR_INVALIDDATA;
        }
    } else {
        /*  8 bpp */
        if (bpp == 8 && r == 0xff && g == 0 && b == 0 && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_GRAY8;
        /* 16 bpp */
        else if (bpp == 16 && r == 0xffff && g == 0 && b == 0 && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_GRAY16LE;
        else if (bpp == 16 && r == 0xf800 && g == 0x7e0 && b == 0x1f && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_RGB565LE;
        /* 24 bpp */
        else if (bpp == 24 && r == 0xff0000 && g == 0xff00 && b == 0xff && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_BGR24;
        /* 32 bpp */
        else if (bpp == 32 && r == 0xff0000 && g == 0xff00 && b == 0xff && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_RGBA; // opaque
        else if (bpp == 32 && r == 0xff && g == 0xff00 && b == 0xff0000 && a == 0)
            avctx->pix_fmt = AV_PIX_FMT_BGRA; // opaque
        else if (bpp == 32 && r == 0xff0000 && g == 0xff00 && b == 0xff && a == 0xff000000)
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
        else if (bpp == 32 && r == 0xff && g == 0xff00 && b == 0xff0000 && a == 0xff000000)
            avctx->pix_fmt = AV_PIX_FMT_BGRA;
        /* give up */
        else {
            av_log(avctx, AV_LOG_ERROR, "Unknown pixel format "
                   "[bpp %d r 0x%x g 0x%x b 0x%x a 0x%x].\n", bpp, r, g, b, a);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int block_nb, int thread_nb)
{
    DDSContext *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    int x = (BLOCK_W * block_nb) % avctx->coded_width;
    int y = BLOCK_H * (BLOCK_W * block_nb / avctx->coded_width);
    uint8_t *p = frame->data[0] + x * PIXEL_SIZE + y * frame->linesize[0];
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_rat;

    ctx->tex_fun(p, frame->linesize[0], d);
    return 0;
}

static int dds_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    DDSContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    uint32_t flags;
    int blocks, left;
    int mipmap, alpha_exp;
    int ret;

    ff_dxtc_decompression_init(&ctx->dxtc);
    bytestream2_init(gbc, avpkt->data, avpkt->size);

    if (bytestream2_get_bytes_left(gbc) < 128) {
        av_log(avctx, AV_LOG_ERROR, "Frame is too small (%d).",
               bytestream2_get_bytes_left(gbc));
        return AVERROR_INVALIDDATA;
    }

    if (bytestream2_get_le32(gbc) != MKTAG('D', 'D', 'S', ' ') ||
        bytestream2_get_le32(gbc) != 124) { // header size
        av_log(avctx, AV_LOG_ERROR, "Invalid DDS header.");
        return AVERROR_INVALIDDATA;
    }

    flags = bytestream2_get_le32(gbc);

    avctx->height = bytestream2_get_le32(gbc);
    avctx->width  = bytestream2_get_le32(gbc);
    ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4 */
    avctx->coded_width  = FFALIGN(avctx->width,  BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, BLOCK_H);

    bytestream2_skip(gbc, 4); // pitch
    bytestream2_skip(gbc, 4); // depth
    mipmap = bytestream2_get_le32(gbc);
    if (mipmap != 0)
        av_log(avctx, AV_LOG_VERBOSE, "File has %d mipmaps.\n", mipmap);

    /* Alternative DDS implementations use reserved1 as custom header. */
    if (bytestream2_get_le32(gbc) == MKTAG('G', 'I', 'M', 'P'))
        av_log(avctx, AV_LOG_WARNING, "Image generated with GIMP-DDS, "
               "not all features are implemented.\n");
    bytestream2_skip(gbc, 4 * 2);
    alpha_exp = bytestream2_get_le32(gbc) == MKTAG('A', 'E', 'X', 'P');
    bytestream2_skip(gbc, 4 * 5);
    if (bytestream2_get_le32(gbc) == MKTAG('N', 'V', 'T', 'T'))
        av_log(avctx, AV_LOG_WARNING, "Image generated with NVidia-Texture-"
               "Tool, not all features are implemented.\n");
    bytestream2_skip(gbc, 4);

    /* Now get the DDPF */
    ret = parse_pixel_format(avctx);
    if (ret < 0)
        return ret;

    bytestream2_skip(gbc, 4); // caps
    bytestream2_skip(gbc, 4); // caps2
    bytestream2_skip(gbc, 4); // caps3
    bytestream2_skip(gbc, 4); // caps4
    bytestream2_skip(gbc, 4); // reserved2

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    if (ctx->compressed) {
        /* Use the decompress function on the texture, one block per thread */
        ctx->tex_data = gbc->buffer;
        blocks = avctx->coded_width * avctx->coded_height / (BLOCK_W * BLOCK_H);
        avctx->execute2(avctx, decompress_texture_thread, frame, NULL, blocks);
        bytestream2_skip(gbc, blocks * BLOCK_W * BLOCK_H);
    } else if (ctx->paletted) {
        /* Use the first 1024 bytes as palette, then copy the rest */
        bytestream2_get_buffer(gbc, frame->data[1], 256 * 4);
        bytestream2_get_buffer(gbc, frame->data[0],
                               frame->linesize[0] * frame->height);

        frame->palette_has_changed = 1;
    } else {
        /* Just copy the necessary data in the buffer */
        bytestream2_get_buffer(gbc, frame->data[0],
                               frame->linesize[0] * frame->height);
    }

    /* This has to be done here because it is used by both rgba and dxt5. */
    if (alpha_exp && avctx->pix_fmt == AV_PIX_FMT_RGBA) {
        int i;

        /* Alpha-exponential mode divides each channel by the maximum R, G or B
         * value, and stores the multiplying factor in the alpha channel. */
        av_log(avctx, AV_LOG_DEBUG, "Alpha-exponential mode.\n");
        for (i = 0; i < frame->linesize[0] * frame->height; i += 4) {
            uint8_t *src = frame->data[0] + i;
            int r = src[0];
            int g = src[1];
            int b = src[2];
            int a = src[3];
            src[0] = r * a / 255;
            src[1] = g * a / 255;
            src[2] = b * a / 255;
            src[3] = 255;
        }
    }

    left = bytestream2_get_bytes_left(gbc);
    if (left != 0)
        av_log(avctx, AV_LOG_DEBUG, "%d trailing bytes.\n", left);

    /* Frame is ready to be output */
    *got_frame = 1;

    return avpkt->size;
}

AVCodec ff_dds_decoder = {
    .name           = "dds",
    .long_name      = NULL_IF_CONFIG_SMALL("DirectDraw Surface image decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DDS,
    .decode         = dds_decode,
    .priv_data_size = sizeof(DDSContext),
    .capabilities   = CODEC_CAP_DR1 | CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE
};
