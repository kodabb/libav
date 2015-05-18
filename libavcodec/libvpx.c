/*
 * Copyright (c) 2013 Guillaume Martres <smarter@ubuntu.com>
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

#include <vpx/vpx_codec.h>

#include "libvpx.h"

int ff_vp9_check_experimental(AVCodecContext *avctx)
{
    if (avctx->strict_std_compliance > FF_COMPLIANCE_EXPERIMENTAL &&
        (vpx_codec_version_major() < 1 ||
         (vpx_codec_version_major() == 1 && vpx_codec_version_minor() < 3))) {
        av_log(avctx, AV_LOG_ERROR,
               "Non-experimental support of VP9 requires libvpx >= 1.3.0\n");
        return AVERROR_EXPERIMENTAL;
    }
    return 0;
}

enum AVPixelFormat ff_vpx_imgfmt_to_pixfmt(vpx_img_fmt_t img)
{
    switch(img) {
    case VPX_IMG_FMT_RGB24:     return AV_PIX_FMT_RGB24;
    case VPX_IMG_FMT_RGB565:    return AV_PIX_FMT_RGB565BE;
    case VPX_IMG_FMT_RGB555:    return AV_PIX_FMT_RGB555BE;
    case VPX_IMG_FMT_UYVY:      return AV_PIX_FMT_UYVY422;
    case VPX_IMG_FMT_YUY2:      return AV_PIX_FMT_YUYV422;
    case VPX_IMG_FMT_YVYU:      return AV_PIX_FMT_YVYU422;
    case VPX_IMG_FMT_BGR24:     return AV_PIX_FMT_BGR24;
    case VPX_IMG_FMT_ARGB:      return AV_PIX_FMT_ARGB;
    case VPX_IMG_FMT_ARGB_LE:   return AV_PIX_FMT_BGRA;
    case VPX_IMG_FMT_RGB565_LE: return AV_PIX_FMT_RGB565LE;
    case VPX_IMG_FMT_RGB555_LE: return AV_PIX_FMT_RGB555LE;
    case VPX_IMG_FMT_I420:      return AV_PIX_FMT_YUV420P;
    case VPX_IMG_FMT_I422:      return AV_PIX_FMT_YUV422P;
    case VPX_IMG_FMT_I444:      return AV_PIX_FMT_YUV444P;
    case VPX_IMG_FMT_I440:      return AV_PIX_FMT_YUV440P;
    case VPX_IMG_FMT_444A:      return AV_PIX_FMT_YUVA444P;
    case VPX_IMG_FMT_I42016:    return AV_PIX_FMT_YUV420P16BE;
    case VPX_IMG_FMT_I42216:    return AV_PIX_FMT_YUV422P16BE;
    case VPX_IMG_FMT_I44416:    return AV_PIX_FMT_YUV444P16BE;
    default:                    return AV_PIX_FMT_NONE;
    }
}
