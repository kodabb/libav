/*
 * innoHeim/Rsupport Screen Capture Codec
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
 * innoHeim/Rsupport Screen Capture Codec decoder
 *
 * Fourcc: ISCC, RSCC
 *
 * Lossless codec, data stored in tiles, with optional delfate compression.
 *
 * Header contains the number of tiles in a frame with the tile coordinates,
 * and it can be deflated or not. Similarly, pixel data comes after the header
 * and a variable size value, and it can be deflated or just raw.
 *
 * Supports: BGRA
 */

#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

#define TILE_SIZE 8

typedef struct RsccContext {
    GetByteContext gbc;
    AVFrame *reference;

    /* zlib interation */
    uint8_t *inflated_buf;
    uLongf inflated_size;
} RsccContext;

typedef struct Tile {
    int x, y;
    int w, h;
} Tile;

static av_cold int rscc_close(AVCodecContext *avctx)
{
    RsccContext *ctx = avctx->priv_data;

    av_frame_free(&ctx->reference);
    av_freep(&ctx->inflated_buf);

    return 0;
}

static av_cold int rscc_init(AVCodecContext *avctx)
{
    RsccContext *ctx = avctx->priv_data;

    /* These needs to be set to estimate uncompressed buffer */
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid image size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Allocate reference frame */
    ctx->reference = av_frame_alloc();
    if (!ctx->reference)
        return AVERROR(ENOMEM);

    avctx->pix_fmt = AV_PIX_FMT_BGRA;

    return 0;
}

static int rscc_decode_frame(AVCodecContext *avctx, void *data,
                                     int *got_frame, AVPacket *avpkt)
{
    RsccContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    AVFrame *frame = data;
    Tile *tiles;
    const uint8_t *pixels, *raw;
    uint8_t *inflated_tiles = NULL;
    int tiles_nb, packed_size, pixel_size = 0;
    int i, ret = 0;

    bytestream2_init(gbc, avpkt->data, avpkt->size);

    /* Size check */
    if (bytestream2_get_bytes_left(gbc) < 12) {
        av_log(avctx, AV_LOG_ERROR, "Packet too small (%d)\n", avpkt->size);
        return AVERROR_INVALIDDATA;
    }

    /* Read number of tiles, and allocate the array */
    tiles_nb = bytestream2_get_le16(gbc);
    tiles = av_malloc(sizeof(Tile) * tiles_nb);
    if (!tiles) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    av_log(avctx, AV_LOG_DEBUG, "Frame with %d tiles.\n", tiles_nb);

    /* When there are more than 5 tiles, they are packed together with
     * a size header. When that size does not match the number of tiles
     * times the tile size, it means it got deflated as well */
    if (tiles_nb > 5) {
        GetByteContext tiles_nb_section;
        uLongf packed_tiles_size;

        if (tiles_nb < 32)
            packed_tiles_size = bytestream2_get_byte(gbc);
        else
            packed_tiles_size = bytestream2_get_le16(gbc);

        ff_dlog(avctx, "packed tiles of size %lu.\n", packed_tiles_size);

        /* If necessary, uncompress tiles, and hijack the bytestream reader */
        if (packed_tiles_size != tiles_nb * TILE_SIZE) {
            uLongf length = tiles_nb * TILE_SIZE;
            inflated_tiles = av_malloc(length);
            if (!inflated_tiles) {
                ret = AVERROR(ENOMEM);
                goto end;
            }

            ret = uncompress(inflated_tiles, &length,
                             gbc->buffer, packed_tiles_size);
            if (ret) {
                av_log(avctx, AV_LOG_ERROR, "Tile deflate error %d.\n", ret);
                ret = AVERROR_UNKNOWN;
                goto end;
            }

            /* Skip the compressed tile section in the main byte reader,
             * and point it to read the newly uncompressed data */
            bytestream2_skip(gbc, packed_tiles_size);
            bytestream2_init(&tiles_nb_section, inflated_tiles, length);
            gbc = &tiles_nb_section;
        }
    }

    /* Fill in array of tiles, keeping track of how many pixels are updated */
    for (i = 0; i < tiles_nb; i++) {
        tiles[i].x = bytestream2_get_le16(gbc);
        tiles[i].w = bytestream2_get_le16(gbc);
        tiles[i].y = bytestream2_get_le16(gbc);
        tiles[i].h = bytestream2_get_le16(gbc);

        pixel_size += tiles[i].w * tiles[i].h * 4;

        ff_dlog(avctx, "tile %d orig(%d,%d) %dx%d.\n",
                i, tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h);

        if (tiles[i].w == 0 || tiles[i].h == 0) {
            av_log(avctx, AV_LOG_ERROR, "invalid tile of %d size %dx%d.\n",
                   i, tiles[i].w, tiles[i].h);
            ret = AVERROR_INVALIDDATA;
            goto end;
        } else if (tiles[i].x + tiles[i].w > avctx->width ||
                   tiles[i].y + tiles[i].h > avctx->height) {
            av_log(avctx, AV_LOG_ERROR,
                   "tile %d out of bounds [(%d,%d) %dx%d] > %dx%d.\n",
                    i, tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h,
                    avctx->width, avctx->height);
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }

    /* Reset the reader in case it had been modified before */
    gbc = &ctx->gbc;

    /* Handle midstream parameter change */
    if (ctx->inflated_size != avctx->width * avctx->height * 4) {
        av_frame_unref(ctx->reference);
        ret = ff_get_buffer(avctx, ctx->reference, AV_GET_BUFFER_FLAG_REF);
        if (ret < 0)
            goto end;

        /* Allocate maximum size possible, a full frame */
        ctx->inflated_size = avctx->width * avctx->height * 4;
        ret = av_reallocp(&ctx->inflated_buf, ctx->inflated_size);
        if (ret < 0) {
            ctx->inflated_size = 0;
            goto end;
        }
    }

    /* Extract how much pixel data the tiles contain */
    if (pixel_size < 0x100)
        packed_size = bytestream2_get_byte(gbc);
    else if (pixel_size < 0x10000)
        packed_size = bytestream2_get_le16(gbc);
    else if (pixel_size < 0x1000000)
        packed_size = bytestream2_get_le24(gbc);
    else
        packed_size = bytestream2_get_le32(gbc);

    ff_dlog(avctx, "pixel_size %d packed_size %d.\n", pixel_size, packed_size);

    /* Get pixels buffer, it may be deflated or just raw */
    if (pixel_size == packed_size) {
        pixels = gbc->buffer;
    } else {
        uLongf len = ctx->inflated_size;
        ret = uncompress(ctx->inflated_buf, &len, gbc->buffer, packed_size);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Pixel deflate error %d.\n", ret);
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        pixels = ctx->inflated_buf;
    }

    /* Pointer to actual pixels, will be updated when data is consumed */
    raw = pixels;
    for (i = 0; i < tiles_nb; i++) {
        uint8_t *dst = ctx->reference->data[0] + ctx->reference->linesize[0] *
                       (avctx->height - tiles[i].y - 1) + tiles[i].x * 4;
        av_image_copy_plane(dst, -1 * ctx->reference->linesize[0],
                            raw, tiles[i].w * 4, tiles[i].w * 4, tiles[i].h);
        raw += tiles[i].w * 4 * tiles[i].h;
    }

    /* Frame is ready to be output */
    ret = av_frame_ref(frame, ctx->reference);
    if (ret < 0)
        goto end;

    /* Keyframe when the number of pixels updated matches the whole surface */
    if (pixel_size == ctx->inflated_size) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->key_frame = 1;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_P;
    }
    *got_frame = 1;

end:
    av_free(inflated_tiles);
    av_free(tiles);

    return ret;
}

AVCodec ff_rscc_decoder = {
    .name           = "rscc",
    .long_name      = NULL_IF_CONFIG_SMALL("innoHeim/Rsupport Screen Capture Codec"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_RSCC,
    .init           = rscc_init,
    .decode         = rscc_decode_frame,
    .close          = rscc_close,
    .priv_data_size = sizeof(RsccContext),
    .capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
