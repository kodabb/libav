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

enum DDSPostProc {
    DDS_NONE = 0,
    DDS_ALPHA_EXP,
    DDS_NORMAL_MAP,
    DDS_RAW_YCOCG,
    DDS_SWAP_ALPHA,
    DDS_SWIZZLE_A2XY,
    DDS_SWIZZLE_RBXG,
    DDS_SWIZZLE_RGXB,
    DDS_SWIZZLE_RXBG,
    DDS_SWIZZLE_RXGB,
    DDS_SWIZZLE_XGBR,
    DDS_SWIZZLE_XRBG,
    DDS_SWIZZLE_XGXR,
} DDSPostProc;

enum DDSDXGIFormat {
    DXGI_FORMAT_R16G16B16A16_TYPELESS       =  9,
    DXGI_FORMAT_R16G16B16A16_FLOAT          = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM          = 11,
    DXGI_FORMAT_R16G16B16A16_UINT           = 12,
    DXGI_FORMAT_R16G16B16A16_SNORM          = 13,
    DXGI_FORMAT_R16G16B16A16_SINT           = 14,

    DXGI_FORMAT_R8G8B8A8_TYPELESS           = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM              = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB         = 29,
    DXGI_FORMAT_R8G8B8A8_UINT               = 30,
    DXGI_FORMAT_R8G8B8A8_SNORM              = 31,
    DXGI_FORMAT_R8G8B8A8_SINT               = 32,

    DXGI_FORMAT_BC1_TYPELESS                = 70,
    DXGI_FORMAT_BC1_UNORM                   = 71,
    DXGI_FORMAT_BC1_UNORM_SRGB              = 72,
    DXGI_FORMAT_BC2_TYPELESS                = 73,
    DXGI_FORMAT_BC2_UNORM                   = 74,
    DXGI_FORMAT_BC2_UNORM_SRGB              = 75,
    DXGI_FORMAT_BC3_TYPELESS                = 76,
    DXGI_FORMAT_BC3_UNORM                   = 77,
    DXGI_FORMAT_BC3_UNORM_SRGB              = 78,
    DXGI_FORMAT_BC4_TYPELESS                = 79,
    DXGI_FORMAT_BC4_UNORM                   = 80,
    DXGI_FORMAT_BC4_SNORM                   = 81,
    DXGI_FORMAT_BC5_TYPELESS                = 82,
    DXGI_FORMAT_BC5_UNORM                   = 83,
    DXGI_FORMAT_BC5_SNORM                   = 84,
    DXGI_FORMAT_B5G6R5_UNORM                = 85,
    DXGI_FORMAT_B8G8R8A8_UNORM              = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM              = 88,
    DXGI_FORMAT_B8G8R8A8_TYPELESS           = 90,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB         = 91,
    DXGI_FORMAT_B8G8R8X8_TYPELESS           = 92,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB         = 93,
} DDSDXGIFormat;

typedef struct DXD3Context {
    TextureDSPContext texdsp;
    GetByteContext gbc;

    int compressed;
    int paletted;
    enum DDSPostProc postproc;

    const uint8_t *tex_data; // Compressed texture
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

    bytestream2_skip(gbc, 4); // dunno
    bytestream2_skip(gbc, 4); // dunno

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    /* Use the decompress function on the texture, one block per thread. */
    ctx->tex_data = gbc->buffer;
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

AVCodec ff_dxd3_decoder = {
    .name           = "dxd3",
    .long_name      = NULL_IF_CONFIG_SMALL("Resolume DXD3"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DXD3,
    .init           = dxd3_init,
    .decode         = dxd3_decode,
    .priv_data_size = sizeof(DXD3Context),
    .capabilities   = CODEC_CAP_DR1 | CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE
};
