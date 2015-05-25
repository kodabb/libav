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

#define DDPF_FOURCC    (1 << 2)
#define DDPF_PALETTE   (1 << 5)
#define DDPF_NORMALMAP (1 << 31)

enum DDSPostProc {
    DDS_NONE = 0,
    DDS_ALPHA_EXP,
    DDS_NORMAL_MAP,
    DDS_DOOM3,
    DDS_RAW_YCOCG,
    DDS_SWAP_ALPHA,
    DDS_A2XY,
} DDSPostProc;

typedef struct DDSContext {
    DXTCContext dxtc;
    GetByteContext gbc;

    int compressed;
    int paletted;
    enum DDSPostProc postproc;

    const uint8_t *tex_data; // Compressed texture
    int tex_ratio;           // Compression ratio

    /* Pointer to the selected compress or decompress function. */
    int (*tex_fun)(uint8_t *dst, ptrdiff_t stride, const uint8_t *block);
} DDSContext;

static int parse_pixel_format(AVCodecContext *avctx)
{
    DDSContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    char buf[32];
    uint32_t flags, fourcc, gimp_tag;
    int size, bpp, r, g, b, a;
    int alpha_exponent, ycocg_classic, ycocg_scaled, normal_map;

    /* Alternative DDS implementations use reserved1 as custom header. */
    bytestream2_skip(gbc, 4 * 3);
    gimp_tag = bytestream2_get_le32(gbc);
    alpha_exponent = gimp_tag == MKTAG('A', 'E', 'X', 'P');
    ycocg_classic  = gimp_tag == MKTAG('Y', 'C', 'G', '1');
    ycocg_scaled   = gimp_tag == MKTAG('Y', 'C', 'G', '2');
    bytestream2_skip(gbc, 4 * 7);

    /* Now the real DDPF starts. */
    size = bytestream2_get_le32(gbc);
    if (size != 32) {
        av_log(avctx, AV_LOG_ERROR, "Invalid pixel format header %d.\n", size);
        return AVERROR_INVALIDDATA;
    }
    flags = bytestream2_get_le32(gbc);
    ctx->compressed = flags & DDPF_FOURCC;
    ctx->paletted   = flags & DDPF_PALETTE;
    normal_map      = flags & DDPF_NORMALMAP;
    fourcc = bytestream2_get_le32(gbc);

    bpp = bytestream2_get_le32(gbc); // rgbbitcount
    r   = bytestream2_get_le32(gbc); // rbitmask
    g   = bytestream2_get_le32(gbc); // gbitmask
    b   = bytestream2_get_le32(gbc); // bbitmask
    a   = bytestream2_get_le32(gbc); // abitmask

    av_get_codec_tag_string(buf, sizeof(buf), fourcc);
    av_log(avctx, AV_LOG_VERBOSE, "fourcc %s bpp %d "
           "r 0x%x g 0x%x b 0x%x a 0x%x.\n", buf, bpp, r, g, b, a);
    if (gimp_tag) {
        av_get_codec_tag_string(buf, sizeof(buf), gimp_tag);
        av_log(avctx, AV_LOG_VERBOSE, "and GIMP-DDS tag %s\n", buf);
    }

    if (ctx->compressed) {
        switch (fourcc) {
        case MKTAG('D', 'X', 'T', '1'):
            ctx->tex_ratio = 8;
            ctx->tex_fun = ctx->dxtc.dxt1a_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '2'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.dxt2_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '3'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.dxt3_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '4'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.dxt4_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('D', 'X', 'T', '5'):
            ctx->tex_ratio = 16;
            if (ycocg_scaled)
                ctx->tex_fun = ctx->dxtc.dxt5ys_block;
            else if (ycocg_classic)
                ctx->tex_fun = ctx->dxtc.dxt5y_block;
            else
                ctx->tex_fun = ctx->dxtc.dxt5_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('R', 'X', 'G', 'B'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.dxt5_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            /* This format may be considered as normal, but it is handled
             * differently in a separate postproc. */
            ctx->postproc = DDS_DOOM3;
            normal_map = 0;
            break;
        case MKTAG('A', 'T', 'I', '1'):
        case MKTAG('B', 'C', '4', 'U'):
            ctx->tex_ratio = 8;
            ctx->tex_fun = ctx->dxtc.rgtc1u_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('B', 'C', '4', 'S'):
            ctx->tex_ratio = 8;
            ctx->tex_fun = ctx->dxtc.rgtc1s_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('A', 'T', 'I', '2'):
        case MKTAG('B', 'C', '5', 'U'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.rgtc2u_block;
            avctx->pix_fmt = AV_PIX_FMT_RGBA;
            break;
        case MKTAG('B', 'C', '5', 'S'):
            ctx->tex_ratio = 16;
            ctx->tex_fun = ctx->dxtc.rgtc2s_block;
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
        case MKTAG('P', '8', ' ', ' '): /* ATI Palette8 */
            ctx->compressed = 0;
            ctx->paletted = 1;
            avctx->pix_fmt = AV_PIX_FMT_PAL8;
            break;
        case MKTAG('A', 'T', 'C', ' '): /* ATI Texture Compression */
        case MKTAG('A', 'T', 'C', 'A'):
        case MKTAG('A', 'T', 'C', 'I'):
        case MKTAG('E', 'T', 'C', ' '): /* Ericsson Texture Compression */
        case MKTAG('E', 'T', 'C', '1'):
        case MKTAG('E', 'T', 'C', '2'):
        case MKTAG('D', 'X', '1', '0'): /* DirectX 10 */
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
        else if (bpp == 16 && r == 0xff && g == 0 && b == 0 && a == 0xff00)
            avctx->pix_fmt = AV_PIX_FMT_YA8;
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

    /* Set any remaining post-proc that should happen before frame is ready. */
    if (alpha_exponent)
        ctx->postproc = DDS_ALPHA_EXP;
    else if (normal_map)
        ctx->postproc = DDS_NORMAL_MAP;
    else if (ycocg_classic && !ctx->compressed)
        ctx->postproc = DDS_RAW_YCOCG;
    else if (avctx->pix_fmt == AV_PIX_FMT_YA8)
        ctx->postproc = DDS_SWAP_ALPHA;

    /* ATI/NVidia variants sometimes add swizzling in bpp. */
    switch (bpp) {
    case MKTAG('A', '2', 'X', 'Y'):
        ctx->postproc = DDS_A2XY;
        break;
    case MKTAG('A', '2', 'D', '5'):
    case MKTAG('x', 'G', 'x', 'R'):
    case MKTAG('x', 'G', 'B', 'R'):
    case MKTAG('x', 'R', 'G', 'B'):
    case MKTAG('R', 'x', 'B', 'G'):
    case MKTAG('R', 'B', 'x', 'G'):
    case MKTAG('R', 'G', 'x', 'B'):
        av_get_codec_tag_string(buf, sizeof(buf), bpp);
        av_log(avctx, AV_LOG_WARNING,
               "Unsupported swizzling type %s, colors might be off.\n", buf);
        break;
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
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_ratio;

    ctx->tex_fun(p, frame->linesize[0], d);
    return 0;
}

/* Convert internal format to normal RGBA (or YA8). */
static void run_postproc(AVCodecContext *avctx, AVFrame *frame)
{
    DDSContext *ctx = avctx->priv_data;
    int i, x_off, y_off;

    switch (ctx->postproc) {
    case DDS_ALPHA_EXP:
        /* Alpha-exponential mode divides each channel by the maximum
         * R, G or B value, and stores the multiplying factor in the
         * alpha channel. */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing alpha exponent.\n");

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
        break;
    case DDS_NORMAL_MAP:
        /* Normal maps work in the XYZ color space and they encode
         * X in R or in A, depending on the texture type, Y in G and
         * derive Z with a square root of the distance.
         *
         * http://www.realtimecollisiondetection.net/blog/?p=28 */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing normal map.\n");

        x_off = ctx->tex_ratio == 8 ? 0 : 3;
        y_off = 1;
        for (i = 0; i < frame->linesize[0] * frame->height; i += 4) {
            uint8_t *src = frame->data[0] + i;
            int x = src[x_off];
            int y = src[y_off];
            int z;
            /* Our data in is [0 255], convert to [-1 1] first. */
            float nx = 2 * x / 255.0f - 1.0f;
            float ny = 2 * y / 255.0f - 1.0f;
            float nz = 0;
            float d = 1.0f - nx * nx - ny * ny;

            if (d > 0)
                nz = sqrtf(d);
            z = av_clip_uint8((int) (255.0f * (nz + 1) / 2));

            src[0] = x;
            src[1] = y;
            src[2] = z;
            src[3] = 255;
        }
        break;
    case DDS_DOOM3:
        /* This format has just R and A swapped. */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing rxgb.\n");

        for (i = 0; i < frame->linesize[0] * frame->height; i += 4) {
            uint8_t *src = frame->data[0] + i;
            FFSWAP(uint8_t, src[0], src[3]);
        }
      break;
    case DDS_RAW_YCOCG:
        /* Data is Y-Co-Cg-A and not RGBA, but they are represented
         * with the same masks in the DDPF header. */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing raw YCoCg.\n");

        for (i = 0; i < frame->linesize[0] * frame->height; i += 4) {
            uint8_t *src = frame->data[0] + i;
            int a  = src[0];
            int cg = src[1] - 128;
            int co = src[2] - 128;
            int y  = src[3];

            src[0] = av_clip_uint8(y + co - cg);
            src[1] = av_clip_uint8(y + cg);
            src[2] = av_clip_uint8(y - co - cg);
            src[3] = a;
        }
        break;
    case DDS_SWAP_ALPHA:
        /* Alpha and Luma are stored swapped. */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing swapped Luma/Alpha.\n");

        for (i = 0; i < frame->linesize[0] * frame->height; i += 2) {
            uint8_t *src = frame->data[0] + i;
            FFSWAP(uint8_t, src[0], src[1]);
        }
        break;
    case DDS_A2XY:
        /* Red and Gree are stored swapped. */
        av_log(avctx, AV_LOG_DEBUG, "Post-processing A2XY swizzle.\n");

        for (i = 0; i < frame->linesize[0] * frame->height; i += 4) {
            uint8_t *src = frame->data[0] + i;
            FFSWAP(uint8_t, src[0], src[1]);
        }
        break;
    }
}

static int dds_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    DDSContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    uint32_t flags;
    int blocks, mipmap;
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
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4. */
    avctx->coded_width  = FFALIGN(avctx->width,  BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, BLOCK_H);

    bytestream2_skip(gbc, 4); // pitch
    bytestream2_skip(gbc, 4); // depth
    mipmap = bytestream2_get_le32(gbc);
    if (mipmap != 0)
        av_log(avctx, AV_LOG_VERBOSE, "Found %d mipmaps (ignored).\n", mipmap);

    /* Extract pixel format information, considering variants in reserved1. */
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
        /* Use the decompress function on the texture, one block per thread. */
        ctx->tex_data = gbc->buffer;
        blocks = avctx->coded_width * avctx->coded_height / (BLOCK_W * BLOCK_H);
        avctx->execute2(avctx, decompress_texture_thread, frame, NULL, blocks);
    } else if (ctx->paletted) {
        /* Use the first 1024 bytes as palette, then copy the rest. */
        bytestream2_get_buffer(gbc, frame->data[1], 256 * 4);
        bytestream2_get_buffer(gbc, frame->data[0],
                               frame->linesize[0] * frame->height);

        frame->palette_has_changed = 1;
    } else if (avctx->pix_fmt == AV_PIX_FMT_UYVY422 ||
               avctx->pix_fmt == AV_PIX_FMT_YUYV422) {
        const uint8_t *src[4];
        int linesizes[4];
        src[0] = gbc->buffer;
        linesizes[0] = frame->width * 2;
        av_image_copy(frame->data, frame->linesize, src, linesizes,
                      avctx->pix_fmt, frame->width, frame->height);
    } else {
        /* Just copy the necessary data in the buffer. */
        bytestream2_get_buffer(gbc, frame->data[0],
                               frame->linesize[0] * frame->height);
    }

    /* Run any post processing here if needed. */
    if (avctx->pix_fmt == AV_PIX_FMT_RGBA || avctx->pix_fmt == AV_PIX_FMT_YA8)
        run_postproc(avctx, frame);

    /* Frame is ready to be output. */
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;
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
