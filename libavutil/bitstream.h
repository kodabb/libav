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

#if 0
#define VLC_TYPE int16_t

typedef struct VLC {
    int bits;
    VLC_TYPE (*table)[2]; ///< code, bits
    int table_size, table_allocated;
} VLC;

typedef struct RL_VLC_ELEM {
    int16_t level;
    int8_t len;
    uint8_t run;
} RL_VLC_ELEM;
#endif

/* Bitstream reader API docs:
 * name
 *   arbitrary name which is used as prefix for the internal variables
 *
 * gb
 *   getbitcontext
 *
 * OPEN_READER(name, gb)
 *   load gb into local variables
 *
 * CLOSE_READER(name, gb)
 *   store local vars in gb
 *
 * UPDATE_CACHE(name, gb)
 *   Refill the internal cache from the bitstream.
 *   After this call at least MIN_CACHE_BITS will be available.
 *
 * GET_CACHE(name, gb)
 *   Will output the contents of the internal cache,
 *   next bit is MSB of 32 or 64 bit (FIXME 64bit).
 *
 * SHOW_UBITS(name, gb, num)
 *   Will return the next num bits.
 *
 * SHOW_SBITS(name, gb, num)
 *   Will return the next num bits and do sign extension.
 *
 * SKIP_BITS(name, gb, num)
 *   Will skip over the next num bits.
 *   Note, this is equivalent to SKIP_CACHE; SKIP_COUNTER.
 *
 * SKIP_CACHE(name, gb, num)
 *   Will remove the next num bits from the cache (note SKIP_COUNTER
 *   MUST be called before UPDATE_CACHE / CLOSE_READER).
 *
 * SKIP_COUNTER(name, gb, num)
 *   Will increment the internal bit counter (see SKIP_CACHE & SKIP_BITS).
 *
 * LAST_SKIP_BITS(name, gb, num)
 *   Like SKIP_BITS, to be used if next call is UPDATE_CACHE or CLOSE_READER.
 *
 * For examples see av_bitstream_get, av_bitstream_show, av_bitstream_skip, get_vlc.
 */

#ifdef LONG_BITSTREAM_READER
#   define MIN_CACHE_BITS 32
#else
#   define MIN_CACHE_BITS 25
#endif

#define OPEN_READER(name, gb)                   \
    unsigned int name ## _index = (gb)->index;  \
    unsigned int av_unused name ## _cache = 0;  \
    unsigned int av_unused name ## _size_plus8 = (gb)->size_in_bits_plus8

#define HAVE_BITS_REMAINING(name, gb) name ## _index < name ## _size_plus8

#define CLOSE_READER(name, gb) (gb)->index = name ## _index

#ifdef BITSTREAM_READER_LE

# ifdef LONG_BITSTREAM_READER
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RL64((gb)->buffer + (name ## _index >> 3)) >> (name ## _index & 7)
# else
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RL32((gb)->buffer + (name ## _index >> 3)) >> (name ## _index & 7)
# endif

# define SKIP_CACHE(name, gb, num) name ## _cache >>= (num)

#else

# ifdef LONG_BITSTREAM_READER
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RB64((gb)->buffer + (name ## _index >> 3)) >> (32 - (name ## _index & 7))
# else
#   define UPDATE_CACHE(name, gb) name ## _cache = \
        AV_RB32((gb)->buffer + (name ## _index >> 3)) << (name ## _index & 7)
# endif

# define SKIP_CACHE(name, gb, num) name ## _cache <<= (num)

#endif

#   define SKIP_COUNTER(name, gb, num) \
    name ## _index = FFMIN(name ## _size_plus8, name ## _index + (num))

#define SKIP_BITS(name, gb, num)                \
    do {                                        \
        SKIP_CACHE(name, gb, num);              \
        SKIP_COUNTER(name, gb, num);            \
    } while (0)

#define LAST_SKIP_BITS(name, gb, num) SKIP_COUNTER(name, gb, num)

#ifdef BITSTREAM_READER_LE
#   define SHOW_UBITS(name, gb, num) zero_extend(name ## _cache, num)
#   define SHOW_SBITS(name, gb, num) sign_extend(name ## _cache, num)
#else
#ifndef NEG_SSR32
#   define NEG_SSR32(a,s) ((( int32_t)(a))>>(32-(s)))
#endif
#ifndef NEG_USR32
#   define NEG_USR32(a,s) (((uint32_t)(a))>>(32-(s)))
#endif
#   define SHOW_UBITS(name, gb, num) NEG_USR32(name ## _cache, num)
#   define SHOW_SBITS(name, gb, num) NEG_SSR32(name ## _cache, num)
#endif

#define GET_CACHE(name, gb) ((uint32_t) name ## _cache)

static inline int av_bitstream_get_count(const AVGetBitContext *s)
{
    return s->index;
}

static inline void av_bitstream_skip_long(AVGetBitContext *s, int n)
{
    s->index += av_clip(n, -s->index, s->size_in_bits_plus8 - s->index);
}

#if 0
/**
 * read mpeg1 dc style vlc (sign bit + mantisse with no MSB).
 * if MSB not set it is negative
 * @param n length in bits
 */
static inline int get_xbits(AVGetBitContext *s, int n)
{
    register int sign;
    register int32_t cache;
    OPEN_READER(re, s);
    UPDATE_CACHE(re, s);
    cache = GET_CACHE(re, s);
    sign  = ~cache >> 31;
    LAST_SKIP_BITS(re, s, n);
    CLOSE_READER(re, s);
    return (NEG_USR32(sign ^ cache, n) ^ sign) - sign;
}

static inline int get_sbits(AVGetBitContext *s, int n)
{
    register int tmp;
    OPEN_READER(re, s);
    UPDATE_CACHE(re, s);
    tmp = SHOW_SBITS(re, s, n);
    LAST_SKIP_BITS(re, s, n);
    CLOSE_READER(re, s);
    return tmp;
}
#endif

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
#if 0
#ifndef sign_extend
static inline av_const int sign_extend(int val, unsigned bits)
{
    unsigned shift = 8 * sizeof(int) - bits;
    union { unsigned u; int s; } v = { (unsigned) val << shift };
    return v.s >> shift;
}
#endif
/**
 * Read 0-32 bits as a signed integer.
 */
static inline int get_sbits_long(AVGetBitContext *s, int n)
{
    return sign_extend(av_bitstream_get_long(s, n), n);
}
#endif

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

//#define TRACE

#ifdef TRACE
#include "log.h"

static inline int av_bitstream_get_trace(AVGetBitContext *s, int n,
                                         const char *file, const char *func,
                                         int line)
{
    int i;
    int bits = av_bitstream_get(s, n);

    for (i = n - 1; i >= 0; i--)
        av_log(NULL, AV_LOG_DEBUG, "%d", (bits >> i) & 1);
    for (i = n; i < 24; i++)
        av_log(NULL, AV_LOG_DEBUG, " ");
    av_log(NULL, AV_LOG_DEBUG, "%5d %2d %3d bit @%5d in %s %s:%d\n",
        bits, n, bits, av_bitstream_get_count(s) - n, file, func, line);

    return bits;
}

#define av_bitstream_get(s, n) \
    av_get_bits_trace(s , n, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#define av_bitstream_get1(s)   \
    av_get_bits_trace(s,  1, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#endif

#endif /* AVUTIL_BITSTREAM_H */


