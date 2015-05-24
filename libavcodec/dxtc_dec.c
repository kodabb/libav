/*
 * DXTC texture decompression
 * Copyright (C) 2009 Benjamin Dobell, Glass Echidna
 * Copyright (C) 2012 - 2015 Matthäus G. "Anteru" Chajdas (http://anteru.net)
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stddef.h>
#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"

#include "dxtc.h"

/**
 * @file
 * DXTC decompression module
 *
 * A description of the algorithm can be found here:
 *   https://www.opengl.org/wiki/S3_Texture_Compression
 *
 * All functions return how much data has been consumed.
 *
 * Pixel output format is always AV_PIX_FMT_RGBA.
 */

static const uint8_t const_black[] = {
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
};

#define RGBA(r, g, b, a) (r) | ((g) << 8) | ((b) << 16) | ((a) << 24)

static av_always_inline void dxt1_block_internal(uint8_t *dst,
                                                 const uint8_t *block,
                                                 ptrdiff_t stride,
                                                 const uint8_t *alpha_tab,
                                                 uint8_t alpha_1bit)
{
    uint32_t tmp, code;
    uint16_t color0, color1;
    uint8_t r0, g0, b0, r1, g1, b1;
    int i, j;

    color0 = AV_RL16(block);
    color1 = AV_RL16(block + 2);

    tmp = (color0 >> 11) * 255 + 16;
    r0  = (uint8_t) ((tmp / 32 + tmp) / 32);
    tmp = ((color0 & 0x07E0) >> 5) * 255 + 32;
    g0  = (uint8_t) ((tmp / 64 + tmp) / 64);
    tmp = (color0 & 0x001F) * 255 + 16;
    b0  = (uint8_t) ((tmp / 32 + tmp) / 32);

    tmp = (color1 >> 11) * 255 + 16;
    r1  = (uint8_t) ((tmp / 32 + tmp) / 32);
    tmp = ((color1 & 0x07E0) >> 5) * 255 + 32;
    g1  = (uint8_t) ((tmp / 64 + tmp) / 64);
    tmp = (color1 & 0x001F) * 255 + 16;
    b1  = (uint8_t) ((tmp / 32 + tmp) / 32);

    code = AV_RL32(block + 4);

    if (color0 > color1) {
        for (j = 0; j < 4; j++) {
            for (i = 0; i < 4; i++) {
                uint8_t alpha = alpha_tab[i + j * 4];
                uint32_t pixel = 0;
                uint32_t pos_code = (code >> 2 * (i + j * 4)) & 0x03;

                switch (pos_code) {
                case 0:
                    pixel = RGBA(r0, g0, b0, alpha);
                    break;
                case 1:
                    pixel = RGBA(r1, g1, b1, alpha);
                    break;
                case 2:
                    pixel = RGBA((2 * r0 + r1) / 3,
                                 (2 * g0 + g1) / 3,
                                 (2 * b0 + b1) / 3,
                                 alpha);
                    break;
                case 3:
                    pixel = RGBA((r0 + 2 * r1) / 3,
                                 (g0 + 2 * g1) / 3,
                                 (b0 + 2 * b1) / 3,
                                 alpha);
                    break;
                }

                AV_WL32(dst + i * 4 + j * stride, pixel);
            }
        }
    } else {
        for (j = 0; j < 4; j++) {
            for (i = 0; i < 4; i++) {
                uint8_t alpha = alpha_tab[i + j * 4];
                uint32_t pixel = 0;
                uint32_t pos_code = (code >> 2 * (i + j * 4)) & 0x03;

                switch (pos_code) {
                case 0:
                    pixel = RGBA(r0, g0, b0, alpha);
                    break;
                case 1:
                    pixel = RGBA(r1, g1, b1, alpha);
                    break;
                case 2:
                    pixel = RGBA((r0 + r1) / 2,
                                 (g0 + g1) / 2,
                                 (b0 + b1) / 2,
                                 alpha);
                    break;
                case 3:
                    pixel = RGBA(0, 0, 0, alpha_1bit);
                    break;
                }

                AV_WL32(dst + i * 4 + j * stride, pixel);
            }
        }
    }
}

/**
 * Decompress one block of a DXT1 texture and store the resulting
 * RGBA pixels in 'dst'. Alpha component is fully opaque.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt1_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    dxt1_block_internal(dst, block, stride, const_black, 255);

    return 8;
}

/**
 * Decompress one block of a DXT1 with 1-bit alpha texture and store
 * the resulting RGBA pixels in 'dst'. Alpha is either fully opaque or
 * fully transparent.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt1a_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    dxt1_block_internal(dst, block, stride, const_black, 0);

    return 8;
}

static av_always_inline void dxt3_block_internal(uint8_t *dst, ptrdiff_t stride,
                                                 const uint8_t *block)
{
    int i;
    uint8_t alpha_values[16] = { 0 };

    for (i = 0; i < 4; i++) {
        const uint16_t alpha = AV_RL16(block);

        alpha_values[i * 4 + 0] = ((alpha >>  0) & 0x0F) * 17;
        alpha_values[i * 4 + 1] = ((alpha >>  4) & 0x0F) * 17;
        alpha_values[i * 4 + 2] = ((alpha >>  8) & 0x0F) * 17;
        alpha_values[i * 4 + 3] = ((alpha >> 12) & 0x0F) * 17;

        block += 2;
    }

    dxt1_block_internal(dst, block, stride, alpha_values, 255);
}

/** Convert a premultiplied alpha pixel to a straigth alpha pixel. */
static inline void premult2straight(uint8_t *src)
{
    int r = src[0];
    int g = src[1];
    int b = src[2];
    int a = src[3];

    src[0] = (uint8_t) r * a / 255;
    src[1] = (uint8_t) g * a / 255;
    src[2] = (uint8_t) b * a / 255;
    src[3] = a;
}

/**
 * Decompress one block of a DXT2 texture and store the resulting
 * RGBA pixels in 'dst'.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt2_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    int x, y;

    dxt3_block_internal(dst, stride, block);

    /* This format is DXT3, but returns premultiplied alpha. It needs to be
     * converted because it's what lavc outputs (and swscale expects). */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            premult2straight(dst + x * 4 + y * stride);

    return 16;
}

/**
 * Decompress one block of a DXT3 texture and store the resulting
 * RGBA pixels in 'dst'.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt3_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    dxt3_block_internal(dst, stride, block);

    return 16;
}

/**
 * Decompress a BC 16x3 index block stored as
 *   h g f e
 *   d c b a
 *   p o n m
 *   l k j i
 *
 * Bits packed as
 *  | h | g | f | e | d | c | b | a | // Entry
 *  |765 432 107 654 321 076 543 210| // Bit
 *  |0000000000111111111112222222222| // Byte
 *
 * into 16 8-bit indices.
 */
static void decompress_indices(uint8_t *dst, const uint8_t *src)
{
    int block, i;

    for (block = 0; block < 2; block++) {
        int tmp = AV_RL24(src);

        /* Unpack 8x3 bit from last 3 byte block */
        for (i = 0; i < 8; i++)
            dst[i] = (tmp >> (i * 3)) & 0x7;

        src += 3;
        dst += 8;
    }
}

static av_always_inline void dxt5_block_internal(uint8_t *dst,
                                                 ptrdiff_t stride,
                                                 const uint8_t *block)
{
    uint8_t alpha0, alpha1;
    uint8_t alpha_indices[16];
    uint8_t r0, g0, b0, r1, g1, b1;
    uint16_t color0, color1;
    uint32_t tmp, code;
    int i, j;

    alpha0 = *(block);
    alpha1 = *(block + 1);

    decompress_indices(alpha_indices, block + 2);

    color0 = AV_RL16(block + 8);
    color1 = AV_RL16(block + 10);

    tmp = (color0 >> 11) * 255 + 16;
    r0  = (uint8_t) ((tmp / 32 + tmp) / 32);
    tmp = ((color0 & 0x07E0) >> 5) * 255 + 32;
    g0  = (uint8_t) ((tmp / 64 + tmp) / 64);
    tmp = (color0 & 0x001F) * 255 + 16;
    b0  = (uint8_t) ((tmp / 32 + tmp) / 32);

    tmp = (color1 >> 11) * 255 + 16;
    r1  = (uint8_t) ((tmp / 32 + tmp) / 32);
    tmp = ((color1 & 0x07E0) >> 5) * 255 + 32;
    g1  = (uint8_t) ((tmp / 64 + tmp) / 64);
    tmp = (color1 & 0x001F) * 255 + 16;
    b1  = (uint8_t) ((tmp / 32 + tmp) / 32);

    code = AV_RL32(block + 12);

    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            int alpha_code = alpha_indices[i + j * 4];
            uint8_t color_code = (code >> 2 * (i + j * 4)) & 0x03;
            uint32_t pixel = 0;
            uint8_t alpha;

            if (alpha_code == 0) {
                alpha = alpha0;
            } else if (alpha_code == 1) {
                alpha = alpha1;
            } else {
                if (alpha0 > alpha1) {
                    alpha = (uint8_t) (((8 - alpha_code) * alpha0 +
                                        (alpha_code - 1) * alpha1) / 7);
                } else {
                    if (alpha_code == 6) {
                        alpha = 0;
                    } else if (alpha_code == 7) {
                        alpha = 255;
                    } else {
                        alpha = (uint8_t) (((6 - alpha_code) * alpha0 +
                                            (alpha_code - 1) * alpha1) / 5);
                    }
                }
            }

            switch (color_code) {
            case 0:
                pixel = RGBA(r0, g0, b0, alpha);
                break;
            case 1:
                pixel = RGBA(r1, g1, b1, alpha);
                break;
            case 2:
                pixel = RGBA((2 * r0 + r1) / 3,
                             (2 * g0 + g1) / 3,
                             (2 * b0 + b1) / 3,
                             alpha);
                break;
            case 3:
                pixel = RGBA((r0 + 2 * r1) / 3,
                             (g0 + 2 * g1) / 3,
                             (b0 + 2 * b1) / 3,
                             alpha);
                break;
            }

            AV_WL32(dst + i * 4 + j * stride, pixel);
        }
    }
}

/**
 * Decompress one block of a DXT4 texture and store the resulting
 * RGBA pixels in 'dst'.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt4_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    int x, y;

    dxt5_block_internal(dst, stride, block);

    /* This format is DXT5, but returns premultiplied alpha. It needs to be
     * converted because it's what lavc outputs (and swscale expects). */
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            premult2straight(dst + x * 4 + y * stride);

    return 16;
}

/**
 * Decompress one block of a DXT5 texture and store the resulting
 * RGBA pixels in 'dst'.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt5_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    dxt5_block_internal(dst, stride, block);

    return 16;
}

/**
 * Convert a YCoCg buffer to RGBA.
 *
 * @param src    input buffer.
 * @param scaled variant with scaled chroma components and opaque alpha.
 */
static void ycocg2rgba(uint8_t *src, int scaled)
{
    int r = src[0];
    int g = src[1];
    int b = src[2];
    int a = src[3];

    int s  = scaled ? (b >> 3) + 1 : 1;
    int y  = a;
    int co = (r - 128) / s;
    int cg = (g - 128) / s;

    src[0] = av_clip_uint8(y + co - cg);
    src[1] = av_clip_uint8(y + cg);
    src[2] = av_clip_uint8(y - co - cg);
    src[3] = scaled ? 255 : b;
}

/**
 * Decompress one block of a DXT5 texture with classic YCoCg and store
 * the resulting RGBA pixels in 'dst'. Alpha component is fully opaque.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt5y_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    int x, y;

    /* This format is basically DXT5, with luma stored in alpha.
     * Run a normal decompress and then reorder the components. */
    dxt5_block_internal(dst, stride, block);

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            ycocg2rgba(dst + x * 4 + y * stride, 0);

    return 16;
}

/**
 * Decompress one block of a DXT5 texture with scaled YCoCg and store
 * the resulting RGBA pixels in 'dst'. Alpha component is fully opaque.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int dxt5ys_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    int x, y;

    /* This format is basically DXT5, with luma stored in alpha.
     * Run a normal decompress and then reorder the components. */
    dxt5_block_internal(dst, stride, block);

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            ycocg2rgba(dst + x * 4 + y * stride, 1);

    return 16;
}

static void rgtc_block_internal(uint8_t *dst, ptrdiff_t stride,
                                const uint8_t *block, const float *color_tab,
                                int sign)
{
    uint8_t indices[16];
    int x, y;

    decompress_indices(indices, block + 2);

    /* Convert from normalized values [-1, 1] or [0, 1] to standard RGBA. */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            int i = indices[x + y * 4];
            int r = (int) (color_tab[i] * 255);
            uint32_t pixel = RGBA(r, 0, 0, 255);
            AV_WL32(dst + x * 4 + y * stride, pixel);
        }
    }
}

/**
 * Decompress one block of a ATI1 texture normalized with unsigned integers
 * and store the resulting RGBA pixels in 'dst'. Alpha is fully opaque.
 *
 * @param dst    output buffer.
 * @param stride scanline in bytes.
 * @param block  block to decompress.
 * @return how much texture data has been consumed.
 */
static int rgtc1u_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *block)
{
    float color_table[8];
    float r0 = block[0] / 255.0f;
    float r1 = block[1] / 255.0f;

    color_table[0] = r0;
    color_table[1] = r1;

    if (r0 > r1) {
        /* 6 interpolated color values */
        color_table[2] = (6 * r0 + 1 * r1) / 7.0f; // bit code 010
        color_table[3] = (5 * r0 + 2 * r1) / 7.0f; // bit code 011
        color_table[4] = (4 * r0 + 3 * r1) / 7.0f; // bit code 100
        color_table[5] = (3 * r0 + 4 * r1) / 7.0f; // bit code 101
        color_table[6] = (2 * r0 + 5 * r1) / 7.0f; // bit code 110
        color_table[7] = (1 * r0 + 6 * r1) / 7.0f; // bit code 111
    } else {
        /* 4 interpolated color values */
        color_table[2] = (4 * r0 + 1 * r1) / 5.0f; // bit code 010
        color_table[3] = (3 * r0 + 2 * r1) / 5.0f; // bit code 011
        color_table[4] = (2 * r0 + 3 * r1) / 5.0f; // bit code 100
        color_table[5] = (1 * r0 + 4 * r1) / 5.0f; // bit code 101
        color_table[6] = 0.0f;                     // bit code 110
        color_table[7] = 1.0f;                     // bit code 111
    }

    rgtc_block_internal(dst, stride, block, color_table, 0);

    return 8;
}

av_cold void ff_dxtc_decompression_init(DXTCContext *c)
{
    c->dxt1_block   = dxt1_block;
    c->dxt1a_block  = dxt1a_block;
    c->dxt2_block   = dxt2_block;
    c->dxt3_block   = dxt3_block;
    c->dxt4_block   = dxt4_block;
    c->dxt5_block   = dxt5_block;
    c->dxt5y_block  = dxt5y_block;
    c->dxt5ys_block = dxt5ys_block;
    c->rgtc1u_block = rgtc1u_block;
}
