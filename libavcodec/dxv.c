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
    int tex_step;       // Distance between blocks
    int tex_size;       // Texture size

    /* Pointer to the selected decompression function */
    int (*tex_funct)(uint8_t *dst, ptrdiff_t stride, const uint8_t *block);
} DXVContext;


#if USE_REP_MOVSB /* small win on amd, big loss on intel */
#if (__i386 || __amd64) && __GNUC__ >= 3
# define lzf_movsb(dst, src, len)                \
   asm ("rep movsb"                              \
        : "=D" (dst), "=S" (src), "=c" (len)     \
        :  "0" (dst),  "1" (src),  "2" (len));
#endif
#endif

# include <errno.h>
# define SET_ERRNO(n) errno = (n)
# define CHECK_INPUT 1

#define u8 uint8_t
static unsigned int 
lzf_decompress (const void *const in_data,  unsigned int in_len,
                void             *out_data, unsigned int out_len)
{
  u8 const *ip = (const u8 *)in_data;
  u8       *op = (u8 *)out_data;
  u8 const *const in_end  = ip + in_len;
  u8       *const out_end = op + out_len;

  do
    {
      unsigned int ctrl = *ip++;

      if (ctrl < (1 << 5)) /* literal run */
        {
          ctrl++;

          if (op + ctrl > out_end)
            {
              SET_ERRNO (E2BIG);
              return 0;
            }

#if CHECK_INPUT
          if (ip + ctrl > in_end)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }
#endif

#ifdef lzf_movsb
          lzf_movsb (op, ip, ctrl);
#else
          switch (ctrl)
            {
              case 32: *op++ = *ip++; case 31: *op++ = *ip++; case 30: *op++ = *ip++; case 29: *op++ = *ip++;
              case 28: *op++ = *ip++; case 27: *op++ = *ip++; case 26: *op++ = *ip++; case 25: *op++ = *ip++;
              case 24: *op++ = *ip++; case 23: *op++ = *ip++; case 22: *op++ = *ip++; case 21: *op++ = *ip++;
              case 20: *op++ = *ip++; case 19: *op++ = *ip++; case 18: *op++ = *ip++; case 17: *op++ = *ip++;
              case 16: *op++ = *ip++; case 15: *op++ = *ip++; case 14: *op++ = *ip++; case 13: *op++ = *ip++;
              case 12: *op++ = *ip++; case 11: *op++ = *ip++; case 10: *op++ = *ip++; case  9: *op++ = *ip++;
              case  8: *op++ = *ip++; case  7: *op++ = *ip++; case  6: *op++ = *ip++; case  5: *op++ = *ip++;
              case  4: *op++ = *ip++; case  3: *op++ = *ip++; case  2: *op++ = *ip++; case  1: *op++ = *ip++;
            }
#endif
        }
      else /* back reference */
        {
          unsigned int len = ctrl >> 5;

          u8 *ref = op - ((ctrl & 0x1f) << 8) - 1;

#if CHECK_INPUT
          if (ip >= in_end)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }
#endif
          if (len == 7)
            {
              len += *ip++;
#if CHECK_INPUT
              if (ip >= in_end)
                {
                  SET_ERRNO (EINVAL);
                  return 0;
                }
#endif
            }

          ref -= *ip++;

          if (op + len + 2 > out_end)
            {
              SET_ERRNO (E2BIG);
              return 0;
            }

          if (ref < (u8 *)out_data)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }

#ifdef lzf_movsb
          len += 2;
          lzf_movsb (op, ref, len);
#else
          switch (len)
            {
              default:
                len += 2;

                if (op >= ref + len)
                  {
                    /* disjunct areas */
                    memcpy (op, ref, len);
                    op += len;
                  }
                else
                  {
                    /* overlapping, use octte by octte copying */
                    do
                      *op++ = *ref++;
                    while (--len);
                  }

                break;

              case 9: *op++ = *ref++;
              case 8: *op++ = *ref++;
              case 7: *op++ = *ref++;
              case 6: *op++ = *ref++;
              case 5: *op++ = *ref++;
              case 4: *op++ = *ref++;
              case 3: *op++ = *ref++;
              case 2: *op++ = *ref++;
              case 1: *op++ = *ref++;
              case 0: *op++ = *ref++; /* two octets more */
                      *op++ = *ref++;
            }
#endif
        }
    }
  while (ip < in_end);

  return op - (u8 *)out_data;
}

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int block_nb, int thread_nb)
{
    DXVContext *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    int x = (TEXTURE_BLOCK_W * block_nb) % (avctx->coded_width);
    int y = TEXTURE_BLOCK_H * (TEXTURE_BLOCK_W * block_nb / (avctx->coded_width));
    uint8_t *p = frame->data[0] + x * 4 + y * frame->linesize[0];
    const uint8_t *d = ctx->tex_data + block_nb * ctx->tex_step;

    ctx->tex_funct(p, frame->linesize[0], d);
    return 0;
}

static void decompress_texture(AVCodecContext *avctx, AVFrame *frame)
{
    DXVContext *ctx = avctx->priv_data;
    int x, y;
    uint8_t *src = ctx->tex_data;

    for (y = 0; y < FFALIGN(avctx->coded_height, 16); y += 4) {
        for (x = 0; x < FFALIGN(avctx->coded_width, 16); x += 4) {
            uint8_t *p = frame->data[0] + x * 4 + y * frame->linesize[0];
            int step = ctx->tex_funct(p, frame->linesize[0], src);
            src += step;
        }
    }
}

/* This scheme addresses already decoded elements depending on 2-bit status:
 *   0 -> copy new element
 *   1 -> copy one element from position -x
 *   2 -> copy one element from position -(get_byte() + 2) * x
 *   3 -> copy one element from position -(get_16le() + 0x102) * x
 * x is 2 for dxt1 and 4 for dxt5. */
#define CHECKPOINT(x)                                                         \
    do {                                                                      \
        if (state == 0) {                                                     \
            value = bytestream2_get_le32(gbc);                                \
            state = 16;                                                       \
        }                                                                     \
        op = value & 0x3;                                                     \
        value >>= 2;                                                          \
        state--;                                                              \
        switch (op) {                                                         \
        case 1: /* copy one element from position -x */                       \
            idx = x;                                                          \
            break;                                                            \
        case 2: /* copy one element from position -(get_byte() + 2) * x */    \
            idx = (bytestream2_get_byte(gbc) + 2) * x;                        \
            break;                                                            \
        case 3: /* copy one element from position -(get_16le() + 0x102) * x */\
            idx = (bytestream2_get_le16(gbc) + 0x102) * x;                    \
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
        CHECKPOINT(2);

        /* Copy two elements from a previous offset or from the input buffer */
        if (op) {
            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
        } else {
            CHECKPOINT(2);

            if (op)
                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            else
                prev = bytestream2_get_le32(gbc);
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            CHECKPOINT(2);

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

static int dxv_decompress_dxt5(AVCodecContext *avctx)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    uint32_t value, op;
    int idx, prev, state = 0;
    int pos = 4;
    int run = 0;
    int probe, check, offset;

    /* Copy the first four elements */
    AV_WL32(ctx->tex_data +  0, bytestream2_get_le32(gbc));
    AV_WL32(ctx->tex_data +  4, bytestream2_get_le32(gbc));
    AV_WL32(ctx->tex_data +  8, bytestream2_get_le32(gbc));
    AV_WL32(ctx->tex_data + 12, bytestream2_get_le32(gbc));

    /* Process input until the whole texture has been filled */
    while (pos < ctx->tex_size / 4) {
        if (run) {
            run--;

            prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
            prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
        } else {
            if (state == 0) {
                value = bytestream2_get_le32(gbc);
                state = 16;
            }
            op = value & 0x3;
            value >>= 2;
            state--;

            switch (op) {
            case 0:
                /* Long copy */
                check = bytestream2_get_byte(gbc) + 1;
                if (check == 256) {
                    do {
                        probe = bytestream2_get_le16(gbc);
                        check += probe;
                    } while (probe == 0xFFFF);
                }
                while (check && pos < ctx->tex_size / 4) {
                    prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                    AV_WL32(ctx->tex_data + 4 * pos, prev);
                    pos++;

                    prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                    AV_WL32(ctx->tex_data + 4 * pos, prev);
                    pos++;

                    prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                    AV_WL32(ctx->tex_data + 4 * pos, prev);
                    pos++;

                    prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                    AV_WL32(ctx->tex_data + 4 * pos, prev);
                    pos++;

                    check--;
                }

                /* Restart (or exit) the loop */
                continue;
                break;
            case 1:
                /* Load new run value */
                run = bytestream2_get_byte(gbc);
                if (run == 255) {
                    do {
                        probe = bytestream2_get_le16(gbc);
                        run += probe;
                    } while (probe == 0xFFFF);
                }

                /* Copy two dwords from previous data */
                prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;

                prev = AV_RL32(ctx->tex_data + 4 * (pos - 4));
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;
                break;
            case 2:
                /* Copy two dwords from a previous index */
                idx = 8 + bytestream2_get_le16(gbc);
                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;

                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;
                break;
            case 3:
                /* Copy two dwords from input */
                prev = bytestream2_get_le32(gbc);
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;

                prev = bytestream2_get_le32(gbc);
                AV_WL32(ctx->tex_data + 4 * pos, prev);
                pos++;
                break;
            }
        }

        CHECKPOINT(4);

        /* Copy two elements from a previous offset or from the input buffer */
        if (op) {
            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;
        } else {
            CHECKPOINT(4);

            if (op)
                prev = AV_RL32(ctx->tex_data + 4 * (pos - idx));
            else
                prev = bytestream2_get_le32(gbc);
            AV_WL32(ctx->tex_data + 4 * pos, prev);
            pos++;

            CHECKPOINT(4);

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

static int dxv_decompress_lzf(AVCodecContext *avctx)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;

    int ret = lzf_decompress(gbc->buffer, bytestream2_get_bytes_left(gbc),
                             ctx->tex_data, ctx->tex_size);
    if (ret == 0) {
        av_log(avctx, AV_LOG_ERROR, "lzf error %s\n.", strerror(errno));
        return -1;
    }

    return 0;
}

static int dxv_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    DXVContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    int (*decompress_tex)(AVCodecContext *avctx);
    const char *compression;
    uint32_t tag;
    int blocks, channels;
    int size = 0, old_type = 0;
    char buf[32];
    int ret;

    bytestream2_init(gbc, avpkt->data, avpkt->size);

    tag = bytestream2_get_le32(gbc);
    av_get_codec_tag_string(buf, sizeof(buf), tag);

    switch (tag) {
    case MKBETAG('D', 'X', 'T', '1'):
        // likely to be default for unknown cases
        ctx->tex_funct = ctx->texdsp.dxt1_block;
        ctx->tex_rat   = 8;
        ctx->tex_step  = 8;
        decompress_tex = dxv_decompress_dxt1;
        av_log(avctx, AV_LOG_VERBOSE, "DXTR1 compression and DXT1 texture\n");
        break;
    case MKBETAG('D', 'X', 'T', '5'):
        ctx->tex_funct = ctx->texdsp.dxt5_block;
        ctx->tex_rat   = 4;
        ctx->tex_step  = 16;
        decompress_tex = dxv_decompress_dxt5;
        av_log(avctx, AV_LOG_VERBOSE, "DXTR5 compression and DXT5 texture\n");
        break;
    case MKBETAG('Y', 'C', 'G', '6'):
    case MKBETAG('Y', 'G', '1', '0'):
    case MKBETAG('U', 'V', 'A', '0'):
        avpriv_report_missing_feature(avctx, "Tag %s (0x%08X)", buf, tag);
        return AVERROR_PATCHWELCOME;
    default:
        size = tag & 0x00FFFFFF;
        old_type = tag >> 24;
        channels = old_type & 0x0F;
        if (old_type & 0x40) {
            av_log(avctx, AV_LOG_VERBOSE, "LZF compression and DXT5 texture\n");
            ctx->tex_funct = ctx->texdsp.dxt5_block;
            ctx->tex_step  = 16;
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "LZF compression and DXT1 texture\n");
            ctx->tex_funct = ctx->texdsp.dxt1_block;
            ctx->tex_step  = 8;
        }
        decompress_tex = dxv_decompress_lzf;
        ctx->tex_rat = 1;
        break;
    }

    /* Version 3 has a 12 byte header instead of a 4 byte one. */
    if (!old_type) {
        channels = bytestream2_get_byte(gbc);
        bytestream2_skip(gbc, 3); // unknown
        size = bytestream2_get_le32(gbc);
    }
    if (size > bytestream2_get_bytes_left(gbc)) {
        av_log(avctx, AV_LOG_ERROR, "Incomplete or invalid file (%u > %u)\n.",
               size, bytestream2_get_bytes_left(gbc));
        return AVERROR_INVALIDDATA;
    }
    av_log(avctx, AV_LOG_TRACE, "%d channels advertised\n", channels);

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    ctx->tex_size = avctx->coded_width * avctx->coded_height * 4 / ctx->tex_rat;
    ret = av_reallocp(&ctx->tex_data, ctx->tex_size);
    if (ret < 0)
        return ret;

    ret = decompress_tex(avctx);
    if (ret < 0)
        return ret;

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

    /* Codec requires 16x16 alignment. */
    avctx->coded_width  = FFALIGN(avctx->width,  16);
    avctx->coded_height = FFALIGN(avctx->height, 16);

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
