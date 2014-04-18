/*
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
 * bitstream reader API header.
 */

#ifndef AVUTIL_BITSTREAM_H
#define AVUTIL_BITSTREAM_H

#include <stdint.h>

#include "common.h"
#include "intreadwrite.h"

typedef struct AVGetBitContext {
    const uint8_t *buffer, *buffer_end;
    int index;
    int size_in_bits;
    int size_in_bits_plus8;
} AVGetBitContext;

typedef struct AVPutBitContext {
    uint32_t bit_buf;
    int bit_left;
    uint8_t *buf, *buf_ptr, *buf_end;
    int size_in_bits;
} AVPutBitContext;

/* Bitstream reader API docs:
 * see lavc/get_bits.h */

#ifdef LONG_BITSTREAM_READER
#   define MIN_CACHE_BITS 32
#else
#   define MIN_CACHE_BITS 25
#endif

#define OPEN_READER(name, gb)                   \
    unsigned int name ## _index = (gb)->index;  \
    unsigned int av_unused name ## _cache = 0;  \
    unsigned int av_unused name ## _size_plus8 = (gb)->size_in_bits_plus8

#define CLOSE_READER(name, gb) (gb)->index = name ## _index

#ifdef BITSTREAM_READER_LE

# ifdef LONG_BITSTREAM_READER
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RL64((gb)->buffer + (name ## _index >> 3)) >> (name ## _index & 7)
# else
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RL32((gb)->buffer + (name ## _index >> 3)) >> (name ## _index & 7)
# endif

#else

# ifdef LONG_BITSTREAM_READER
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RB64((gb)->buffer + (name ## _index >> 3)) >> (32 - (name ## _index & 7))
# else
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RB32((gb)->buffer + (name ## _index >> 3)) << (name ## _index & 7)
# endif

#endif

#define LAST_SKIP_BITS(name, gb, num) \
    name ## _index = FFMIN(name ## _size_plus8, name ## _index + (num))

#ifdef BITSTREAM_READER_LE
#   define av_zero_extend(val, bits) \
        (val << ((8 * sizeof(int)) - bits)) >> ((8 * sizeof(int)) - bits);
#   define SHOW_UBITS(name, gb, num) av_zero_extend(name ## _cache, num)
#else
#   define AV_NEG_USR32(a,s) (((uint32_t) (a)) >> (32 - (s)))
#   define SHOW_UBITS(name, gb, num) AV_NEG_USR32(name ## _cache, num)
#endif

static inline int av_bitstream_get_count(const AVGetBitContext *s)
{
    return s->index;
}

static inline void av_bitstream_skip_long(AVGetBitContext *s, int n)
{
    s->index += av_clip(n, -s->index, s->size_in_bits_plus8 - s->index);
}

/**
 * Read 1-25 bits.
 */
static inline unsigned int av_bitstream_get(AVGetBitContext *s, int n)
{
    register int tmp;
    OPEN_READER(re, s);
    UPDATE_CACHE(re, s);
    tmp = SHOW_UBITS(re, s, n);
    LAST_SKIP_BITS(re, s, n);
    CLOSE_READER(re, s);
    return tmp;
}

/**
 * Show 1-25 bits.
 */
static inline unsigned int av_bitstream_show(AVGetBitContext *s, int n)
{
    register int tmp;
    OPEN_READER(re, s);
    UPDATE_CACHE(re, s);
    tmp = SHOW_UBITS(re, s, n);
    return tmp;
}

static inline void av_bitstream_skip(AVGetBitContext *s, int n)
{
    OPEN_READER(re, s);
    UPDATE_CACHE(re, s);
    LAST_SKIP_BITS(re, s, n);
    CLOSE_READER(re, s);
}

static inline unsigned int av_bitstream_get1(AVGetBitContext *s)
{
    unsigned int index = s->index;
    uint8_t result     = s->buffer[index >> 3];
#ifdef BITSTREAM_READER_LE
    result >>= index & 7;
    result  &= 1;
#else
    result <<= index & 7;
    result >>= 8 - 1;
#endif
    if (s->index < s->size_in_bits_plus8)
        index++;
    s->index = index;

    return result;
}

static inline unsigned int av_bitstream_show1(AVGetBitContext *s)
{
    return av_bitstream_show(s, 1);
}

static inline void av_bitstream_skip1(AVGetBitContext *s)
{
    av_bitstream_skip(s, 1);
}

/**
 * Read 0-32 bits.
 */
static inline unsigned int av_bitstream_get_long(AVGetBitContext *s, int n)
{
    if (n <= MIN_CACHE_BITS) {
        return av_bitstream_get(s, n);
    } else {
#ifdef BITSTREAM_READER_LE
        int ret = av_bitstream_get(s, 16);
        return ret | (av_bitstream_get(s, n - 16) << 16);
#else
        int ret = av_bitstream_get(s, 16) << (n - 16);
        return ret | av_bitstream_get(s, n - 16);
#endif
    }
}

/*
 * Read 0-64 bits.
 */
static inline uint64_t av_bitstream_get64(AVGetBitContext *s, int n)
{
    if (n <= 32) {
        return av_bitstream_get_long(s, n);
    } else {
#ifdef BITSTREAM_READER_LE
        uint64_t ret = av_bitstream_get_long(s, 32);
        return ret | (uint64_t) av_bitstream_get_long(s, n - 32) << 32;
#else
        uint64_t ret = (uint64_t) av_bitstream_get_long(s, n - 32) << 32;
        return ret | av_bitstream_get_long(s, 32);
#endif
    }
}

/**
 * Show 0-32 bits.
 */
static inline unsigned int av_bitstream_show_long(AVGetBitContext *s, int n)
{
    if (n <= MIN_CACHE_BITS) {
        return av_bitstream_show(s, n);
    } else {
        AVGetBitContext gb = *s;
        return av_bitstream_get_long(&gb, n);
    }
}

/**
 * Initialize AVGetBitContext.
 * @param buffer bitstream buffer, must be FF_INPUT_BUFFER_PADDING_SIZE bytes
 *        larger than the actual read bits because some optimized bitstream
 *        readers read 32 or 64 bit at once and could read over the end
 * @param bit_size the size of the buffer in bits
 * @return 0 on success, AVERROR_INVALIDDATA if the buffer_size would overflow.
 */
static inline int av_bitstream_get_init(AVGetBitContext *s,
                                        const uint8_t *buffer,
                                        int bit_size)
{
    int buffer_size;
    int ret = 0;

    if (bit_size > INT_MAX - 7 || bit_size < 0 || !buffer) {
        buffer_size = bit_size = 0;
        buffer      = NULL;
        ret         = AVERROR_INVALIDDATA;
    }

    buffer_size = (bit_size + 7) >> 3;

    s->buffer             = buffer;
    s->size_in_bits       = bit_size;
    s->size_in_bits_plus8 = bit_size + 8;
    s->buffer_end         = buffer + buffer_size;
    s->index              = 0;

    return ret;
}

/**
 * Initialize AVGetBitContext.
 * @param buffer bitstream buffer, must be FF_INPUT_BUFFER_PADDING_SIZE bytes
 *        larger than the actual read bits because some optimized bitstream
 *        readers read 32 or 64 bit at once and could read over the end
 * @param byte_size the size of the buffer in bytes
 * @return 0 on success, AVERROR_INVALIDDATA if the buffer_size would overflow.
 */
static inline int av_bitstream_get_init8(AVGetBitContext *s,
                                         const uint8_t *buffer,
                                         int byte_size)
{
    if (byte_size > INT_MAX / 8)
        return AVERROR_INVALIDDATA;
    return av_bitstream_get_init(s, buffer, byte_size * 8);
}

static inline const uint8_t *align_av_bitstream_get(AVGetBitContext *s)
{
    int n = -av_bitstream_get_count(s) & 7;
    if (n)
        av_bitstream_skip(s, n);
    return s->buffer + (s->index >> 3);
}

static inline int av_bitstream_get_left(AVGetBitContext *gb)
{
    return gb->size_in_bits - av_bitstream_get_count(gb);
}

/**
 * Initialize the AVPutBitContext s.
 *
 * @param buffer the buffer where to put bits
 * @param buffer_size the size in bytes of buffer
 */
static inline void init_av_bitstream_put(AVPutBitContext *s, uint8_t *buffer,
                                 int buffer_size)
{
    if (buffer_size < 0) {
        buffer_size = 0;
        buffer      = NULL;
    }

    s->size_in_bits = 8 * buffer_size;
    s->buf          = buffer;
    s->buf_end      = s->buf + buffer_size;
    s->buf_ptr      = s->buf;
    s->bit_left     = 32;
    s->bit_buf      = 0;
}

/**
 * Pad the end of the output stream with zeros.
 */
static inline void flush_av_bitstream_put(AVPutBitContext *s)
{
#ifndef BITSTREAM_WRITER_LE
    if (s->bit_left < 32)
        s->bit_buf <<= s->bit_left;
#endif
    while (s->bit_left < 32) {
        /* XXX: should test end of buffer */
#ifdef BITSTREAM_WRITER_LE
        *s->buf_ptr++ = s->bit_buf;
        s->bit_buf  >>= 8;
#else
        *s->buf_ptr++ = s->bit_buf >> 24;
        s->bit_buf  <<= 8;
#endif
        s->bit_left  += 8;
    }
    s->bit_left = 32;
    s->bit_buf  = 0;
}

/**
 * Write up to 31 bits into a bitstream.
 * Use av_bitstream_put32 to write 32 bits.
 */
static inline void av_bitstream_put(AVPutBitContext *s, int n, unsigned int value)
{
    unsigned int bit_buf;
    int bit_left;

    assert(n <= 31 && value < (1U << n));

    bit_buf  = s->bit_buf;
    bit_left = s->bit_left;

    /* XXX: optimize */
#ifdef BITSTREAM_WRITER_LE
    bit_buf |= value << (32 - bit_left);
    if (n >= bit_left) {
        AV_WL32(s->buf_ptr, bit_buf);
        s->buf_ptr += 4;
        bit_buf     = (bit_left == 32) ? 0 : value >> bit_left;
        bit_left   += 32;
    }
    bit_left -= n;
#else
    if (n < bit_left) {
        bit_buf     = (bit_buf << n) | value;
        bit_left   -= n;
    } else {
        bit_buf   <<= bit_left;
        bit_buf    |= value >> (n - bit_left);
        AV_WB32(s->buf_ptr, bit_buf);
        s->buf_ptr += 4;
        bit_left   += 32 - n;
        bit_buf     = value;
    }
#endif

    s->bit_buf  = bit_buf;
    s->bit_left = bit_left;
}


/**
 * Write exactly 32 bits into a bitstream.
 */
static inline void av_bitstream_put32(AVPutBitContext *s, uint32_t value)
{
    int lo = value & 0xffff;
    int hi = value >> 16;
#ifdef BITSTREAM_WRITER_LE
    av_bitstream_put(s, 16, lo);
    av_bitstream_put(s, 16, hi);
#else
    av_bitstream_put(s, 16, hi);
    av_bitstream_put(s, 16, lo);
#endif
}

/**
 * Return the pointer to the byte where the bitstream writer will put
 * the next bit.
 */
static inline uint8_t *av_bitstream_put_ptr(AVPutBitContext *s)
{
    return s->buf_ptr;
}


/**
 * @return the total number of bits written to the bitstream.
 */
static inline int av_bitstream_put_count(AVPutBitContext *s)
{
    return (s->buf_ptr - s->buf) * 8 + 32 - s->bit_left;
}

/**
 * read unsigned exp golomb code.
 */
static inline int av_bitstream_get_ue(AVGetBitContext *gb)
{
    int nbits = 0;
    do {
        nbits = av_bitstream_get1(gb);
    } while (!nbits);

    return (av_bitstream_get(gb, nbits) | (1 << nbits + 1) ) - 1;
}

/**
 * read signed exp golomb code.
 */
static inline int av_bitstream_get_se(AVGetBitContext *gb)
{
    int num = av_bitstream_get_ue(gb);
    if (num & 1)
        num = (num + 1) >> 1;
    else
        num = -(num >> 1);
    return num;
}

#endif /* AVUTIL_BITSTREAM_H */
