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

typedef struct DXVContext {
    TextureDSPContext texdsp;
    GetByteContext gbc;

    int compressed;
    int paletted;

    uint8_t *tex_data; // Compressed texture
    int tex_ratio;           // Compression ratio
    int tex_size;           // Compression ratio

    /* Pointer to the selected compress or decompress function. */
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
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_ratio;

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


static const uint16_t DXTR_DistMask[] = {
   0, 0, 0xFF, 0xFFFF
};

static const int DXTR_DistCost[] = {
    0, 0, 1, 2
};

static const int DXTR_DistOffset[] = {
    0, 1, 2, 258
};

#define _BYTE uint8_t
#define _DWORD uint32_t
#define _WORD uint16_t
#if 1
static int DXTR_uncompressDXT1(uint32_t *dst, const uint8_t *inbuf, size_t a4)
{
  _BYTE *src_; // eax@1
  _DWORD *out; // edx@2
  unsigned int state; // ecx@2
  unsigned int v7; // ebx@4
  int v8; // ecx@6
  unsigned int v9; // ecx@8
  uint16_t v10; // si@9
  int v11; // esi@9
  _DWORD *v12; // ecx@9
  int v13; // esi@9
  int v14; // ebx@11
  _BYTE *src; // eax@12
  uint16_t v16; // si@13
  int v17; // ecx@17
  uint16_t v18; // si@19

  src_ = inbuf + 8;
  *dst = *(_DWORD *)inbuf;
  dst[1] = *((_DWORD *)inbuf + 1);
  if ( a4 >= 9 )
  {
    out = dst + 2;
    state = 2;
    while ( 1 )
    {
      if ( state == 2 )
      {
        state = *(_DWORD *)src_;
        src_ += 4;
        v7 = (state >> 2) | 0x80000000;
      }
      else
      {
        v7 = state >> 2;
      }
      v8 = state & 3;
      if ( v8 )
        break;
      if ( v7 == 2 )
      {
        v7 = *(_DWORD *)src_;
        src_ += 4;
        v9 = (v7 >> 2) | 0x80000000;
      }
      else
      {
        v9 = v7 >> 2;
      }
      v14 = v7 & 3;
      if ( v14 )
      {
        v16 = *(_WORD *)src_ & DXTR_DistMask[v14];
        src = &src_[DXTR_DistCost[v14]];
        *out = out[-2 * (DXTR_DistOffset[v14] + v16)];
      }
      else
      {
        *out = *(_DWORD *)src_;
        src = src_ + 4;
      }
      if ( v9 == 2 )
      {
        v9 = *(_DWORD *)src;
        src += 4;
        v7 = (v9 >> 2) | 0x80000000;
      }
      else
      {
        v7 = v9 >> 2;
      }
      v17 = v9 & 3;
      if ( v17 )
      {
        v18 = *(_WORD *)src & DXTR_DistMask[v17];
        src_ = &src[DXTR_DistCost[v17]];
        v13 = -8 * (DXTR_DistOffset[v17] + v18);
        goto LABEL_20;
      }
      out[1] = *(_DWORD *)src;
      src_ = src + 4;
LABEL_21:
      out += 2;
      state = v7;
      if ( out >= (_DWORD *)((char *)dst + a4) )
        return src_ - inbuf;
    }
    v10 = *(_WORD *)src_ & DXTR_DistMask[v8];
    src_ += DXTR_DistCost[v8];
    v11 = 8 * (DXTR_DistOffset[v8] + v10);
    v12 = &out[-v11 / 4u];
    v13 = -v11;
    *out = *v12;
LABEL_20:
    out[1] = *(_DWORD *)((char *)out + v13 + 4);
    goto LABEL_21;
  }
  return src_ - inbuf;
}
#else
//static int DXTR_uncompressDXT1(uint32_t *dst, const uint8_t *inbuf, size_t a4)
#if 0
#include "libavcodec/mathops.h"
#define sign_extend_64(x) sign_extend(x, 64)
static int DXTR_uncompressDXT1(int arg2, int arg0, int arg3) {
    int var_8 = arg0;
    int var_38 = arg2 + arg3;
    int var_3C = 0x2;
    *(int32_t *)arg2 = *(int32_t *)var_8;
    *(int32_t *)(arg2 + 0x4) = *(int32_t *)(var_8 + 0x4);
    int var_30 = arg2 + 0x8;
    int var_28 = var_8 + 0x8;
    int var_40, var_50, var_60, var_70;
    int rcx, rsi,rax;

    while (var_30 < var_38) {
            if (var_3C == 0x2) {
                    rcx = *(int32_t *)var_28;
                    var_28 = var_28 + 0x4;
                    var_40 = rcx & 0x3;
                    var_3C = (rcx >> 0x2) + 0x80000000;
            }
            else {
                    var_40 = var_3C & 0x3;
                    var_3C = var_3C >> 0x2;
            }
            if (var_40 != 0x0) {
                    rsi = sign_extend_64(*(int16_t *)var_28 & 0xffff & *(int16_t *)(DXTR_DistMask + sign_extend_64(var_40) * 0x2) & 0xffff) + *(DXTR_DistOffset + sign_extend_64(var_40) * 0x8);
                    var_28 = var_28 + *(DXTR_DistCost + sign_extend_64(var_40) * 0x8);
                    *(int32_t *)var_30 = *(int32_t *)(var_30 + (0x0 - (rsi << 0x3)));
                    var_50 = 0x4 + var_30 + (0x0 - (rsi << 0x3));
                    *(int32_t *)(var_30 + 0x4) = *(int32_t *)var_50;
                    var_30 = var_30 + 0x8;
            }
            else {
                    if (var_3C == 0x2) {
                            rcx = *(int32_t *)var_28;
                            var_28 = var_28 + 0x4;
                            var_40 = rcx & 0x3;
                            var_3C = (rcx >> 0x2) + 0x80000000;
                    }
                    else {
                            var_40 = var_3C & 0x3;
                            var_3C = var_3C >> 0x2;
                    }
                    if (var_40 != 0x0) {
                            var_60 = var_30 + (0x0 - (sign_extend_64(*(int16_t *)var_28 & 0xffff & *(int16_t *)(DXTR_DistMask + sign_extend_64(var_40) * 0x2) & 0xffff) + *(DXTR_DistOffset + sign_extend_64(var_40) * 0x8) << 0x3));
                            var_28 = var_28 + *(DXTR_DistCost + sign_extend_64(var_40) * 0x8);
                            *(int32_t *)var_30 = *(int32_t *)var_60;
                            var_30 = var_30 + 0x4;
                    }
                    else {
                            *(int32_t *)var_30 = *(int32_t *)var_28;
                            var_30 = var_30 + 0x4;
                            var_28 = var_28 + 0x4;
                    }
                    if (var_3C == 0x2) {
                            rcx = *(int32_t *)var_28;
                            var_28 = var_28 + 0x4;
                            var_40 = rcx & 0x3;
                            var_3C = (rcx >> 0x2) + 0x80000000;
                    }
                    else {
                            var_40 = var_3C & 0x3;
                            var_3C = var_3C >> 0x2;
                    }
                    if (var_40 != 0x0) {
                            var_70 = var_30 + (0x0 - (sign_extend_64(*(int16_t *)var_28 & 0xffff & *(int16_t *)(DXTR_DistMask + sign_extend_64(var_40) * 0x2) & 0xffff) + *(DXTR_DistOffset + sign_extend_64(var_40) * 0x8) << 0x3));
                            var_28 = var_28 + *(DXTR_DistCost + sign_extend_64(var_40) * 0x8);
                            *(int32_t *)var_30 = *(int32_t *)var_70;
                            var_30 = var_30 + 0x4;
                    }
                    else {
                            *(int32_t *)var_30 = *(int32_t *)var_28;
                            var_30 = var_30 + 0x4;
                            var_28 = var_28 + 0x4;
                    }
            }
    }
    rax = var_28 - var_8;
    return rax;
}
#else
static int64_t DXTR_uncompressDXT1(uint64_t *dst, uint8_t *inbuf, int64_t size)
{
  unsigned int v4; // ST34_4@4
  int64_t v5; // ST20_8@7
  int64_t v6; // ST40_8@7
  unsigned int v7; // ST34_4@9
  int64_t v8; // ST18_8@12
  unsigned int v9; // ST34_4@15
  int64_t v10; // ST08_8@18
  int v12; // [sp+30h] [bp-40h]@4
  int v13; // [sp+30h] [bp-40h]@9
  int v14; // [sp+30h] [bp-40h]@15
  unsigned int status; // [sp+34h] [bp-3Ch]@1
  unsigned int v16; // [sp+34h] [bp-3Ch]@9
  int64_t src_; // [sp+40h] [bp-30h]@1
  int64_t v18; // [sp+40h] [bp-30h]@12
  int64_t out; // [sp+48h] [bp-28h]@1
  int64_t v20; // [sp+48h] [bp-28h]@12

  status = 2;
  *(_DWORD *)dst = *(_DWORD *)inbuf;
  *(_DWORD *)(dst + 4) = *(_DWORD *)(inbuf + 4);
  src_ = dst + 8;
  out = inbuf + 8;
  while ( src_ < (uint64_t)(size + dst) )
  {
    if ( status == 2 )
    {
      v4 = *(_DWORD *)out;
      out += 4LL;
      v12 = v4 & 3;
      status = (v4 >> 2) + 2147483648;
    }
    else
    {
      v12 = status & 3;
      status >>= 2;
    }
    if ( v12 )
    {
      v5 = -8 * (DXTR_DistOffset[v12] + (uint16_t)(DXTR_DistMask[v12] & *(_WORD *)out)) + src_;
      out += DXTR_DistCost[v12];
      *(_DWORD *)src_ = *(_DWORD *)v5;
      v6 = src_ + 4;
      *(_DWORD *)v6 = *(_DWORD *)(v5 + 4);
      src_ = v6 + 4;
    }
    else
    {
      if ( status == 2 )
      {
        v7 = *(_DWORD *)out;
        out += 4LL;
        v13 = v7 & 3;
        v16 = (v7 >> 2) + 2147483648;
      }
      else
      {
        v13 = status & 3;
        v16 = status >> 2;
      }
      if ( v13 )
      {
        v8 = DXTR_DistOffset[v13] + (uint16_t)(DXTR_DistMask[v13] & *(_WORD *)out);
        v20 = DXTR_DistCost[v13] + out;
        *(_DWORD *)src_ = *(_DWORD *)(-8 * v8 + src_);
        v18 = src_ + 4;
      }
      else
      {
        *(_DWORD *)src_ = *(_DWORD *)out;
        v18 = src_ + 4;
        v20 = out + 4;
      }
      if ( v16 == 2 )
      {
        v9 = *(_DWORD *)v20;
        v20 += 4LL;
        v14 = v9 & 3;
        status = (v9 >> 2) + 2147483648;
      }
      else
      {
        v14 = v16 & 3;
        status = v16 >> 2;
      }
      if ( v14 )
      {
        v10 = DXTR_DistOffset[v14] + (uint16_t)(DXTR_DistMask[v14] & *(_WORD *)v20);
        out = DXTR_DistCost[v14] + v20;
        *(_DWORD *)v18 = *(_DWORD *)(-8 * v10 + v18);
        src_ = v18 + 4;
      }
      else
      {
        *(_DWORD *)v18 = *(_DWORD *)v20;
        src_ = v18 + 4;
        out = v20 + 4;
      }
    }
  }
  return out - inbuf;
}
#endif
#endif

static int dxv_decode(AVCodecContext *avctx, void *data,
                       int *got_frame, AVPacket *avpkt)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    uint32_t version, tag;
    int ret, blocks, size, consumed;

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
    avctx->pix_fmt = AV_PIX_FMT_RGBA;
    ctx->tex_funct = ctx->texdsp.dxt1_block;
    ctx->tex_ratio = 16;

    version = bytestream2_get_le32(gbc);
    if (version != 4) {
        av_log(avctx, AV_LOG_WARNING, "version %d\n", version);
    }
    size = bytestream2_get_le32(gbc);
    if (size != bytestream2_get_bytes_left(gbc)) {
        av_log(avctx, AV_LOG_ERROR, "Incomplete file (%u > %u)\n.",
               size, bytestream2_get_bytes_left(gbc));
    }

    av_log(avctx, AV_LOG_WARNING, "New frame!\n");

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    ctx->tex_size = avctx->coded_width * avctx->coded_height * 4 / 8;
    ret = av_reallocp(&ctx->tex_data, ctx->tex_size);
    if (ret < 0)
        return ret;

    consumed = DXTR_uncompressDXT1(ctx->tex_data, gbc->buffer, ctx->tex_size);
    av_log(avctx, AV_LOG_WARNING, "Consumed %d bytes over %d (left %d)\n", consumed, size, size - consumed);

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
