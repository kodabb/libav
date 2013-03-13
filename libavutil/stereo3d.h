/*
 * Copyright (c) 2013 Vittorio Giovara <vittorio.giovara@gmail.com>
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

/**
 * List of possible 3D Types
 */
enum AVStereo3DType {
    /**
     * No information about stereoscopy.
     */
    AV_STEREO3D_UNKNOWN,

    /**
     * Video is not stereoscopic (and metadata has to be there).
     */
    AV_STEREO3D_2D,

    /**
     * Views are alternated temporally.
     *
     *     frame0   frame1   frame2   ...
     *    LLLLLLLL RRRRRRRR LLLLLLLL
     *    LLLLLLLL RRRRRRRR LLLLLLLL
     *    LLLLLLLL RRRRRRRR LLLLLLLL
     *    ...      ...      ...
     */
    AV_STEREO3D_FRAMESEQUENCE,

    /**
     * Views are packed in a checkerboard-like structure per pixel.
     *
     *    LRLRLRLR
     *    RLRLRLRL
     *    LRLRLRLR
     *    ...
     */
    AV_STEREO3D_CHECKERS,

    /**
     * Views are packed per line, as if interlaced.
     *
     *    LLLLLLLL
     *    RRRRRRRR
     *    LLLLLLLL
     *    ...
     */
    AV_STEREO3D_LINES,

    /**
     * Views are packed per column.
     *
     *    LRLRLRLR
     *    LRLRLRLR
     *    LRLRLRLR
     *    ...
     */
    AV_STEREO3D_COLUMNS,

    /**
     * Views are next to each other.
     *
     *    LLLLRRRR
     *    LLLLRRRR
     *    LLLLRRRR
     *    ...
     */
    AV_STEREO3D_SIDEBYSIDE,

    /**
     * Views are on top of each other.
     *
     *    LLLLLLLL
     *    LLLLLLLL
     *    RRRRRRRR
     *    RRRRRRRR
     */
    AV_STEREO3D_TOPBOTTOM,

    /**
     * Views are split across the frame.
     *
     *    LLLLLLRRR
     *    LLLLLLRRR
     *    RRRRRRXXX
     */
    AV_STEREO3D_TILES,

    /**
     * Views are colored funny, as described in AVStereo3DAnaglyph.
     */
    AV_STEREO3D_ANAGLYPH,

    /**
     * Views are in two different streams: this could be per container
     * (like Matroska) or per frame (like MVC Stereo High Profile).
     */
    AV_STEREO3D_MULTISTREAM,
};

enum AVStereo3DInfo {
    /**
     * Views are assumed to be two and completely contained within
     * the frame with Left/Top representing the left view.
     */
    AV_STEREO3D_NORMAL            = 0x0000,

    /**
     * Views are at full resolution (no upsampling needed).
     */
    AV_STEREO3D_SIZE_FULL         = 0x0001,

    /**
     * Inverted views, Right/Bottom represent the left view.
     */
    AV_STEREO3D_ORDER_INVERT      = 0x0002,

    /**
     * When upscaling apply a checkerboard pattern.
     *
     *     LLLLRRRR          L L L L    R R R R
     *     LLLLRRRR    =>     L L L L  R R R R
     *     LLLLRRRR          L L L L    R R R R
     *     LLLLRRRR           L L L L  R R R R
     *
     * @note AV_STEREO3D_SIZE_FULL should not be set.
     */
    AV_STEREO3D_SAMPLE_QUINCUNX   = 0x0004,
};

/**
 * List of possible Anaglyph modes.
 */
enum AVStereo3DAnaglyph {
    AV_STEREO3D_ANAGLYPH_UNKNOWN,
    AV_STEREO3D_ANAGLYPH_RED_CYAN,
    AV_STEREO3D_ANAGLYPH_RED_GREEN,
    AV_STEREO3D_ANAGLYPH_RED_BLUE,
    AV_STEREO3D_ANAGLYPH_GREEN_MAGENTA,
    AV_STEREO3D_ANAGLYPH_AMBER_BLUE,
    AV_STEREO3D_ANAGLYPH_MAGENTA_CYAN,
};

/**
 * Stereo 3D type: this specifies how a stereo pair or mulview video
 * is packed in a frame or in a container. Usually this information
 * is found in the container header or at every keyframe.
 */
typedef struct AVStereo3D {
    /**
     * How views are packed within the frame/container.
     */
    enum AVStereo3DType type;

    /**
     * Additional information about the packing.
     */
    enum AVStereo3DInfo info;

    /**
     * Anaglyph type.
     */
    enum AVStereo3DAnaglyph anaglyph;
} AVStereo3D;

/**
 * Allocate an AVStereo3D structure and set its fields to default values.
 * The resulting struct must be freed using av_stereo3d_free().
 *
 * @return An AVStereo3D filled with default values or NULL on failure.
 */
AVStereo3D *av_stereo3d_alloc(void);

/**
 * Free the AVStereo3D structure.
 *
 * @param steero struct to be freed. The pointer will be set to NULL.
 */
void av_stereo3d_free(AVStereo3D **data);

/**
 * Return a human-readable name of the stereo type.
 */
const char *av_stereo3d_name(const enum AVStereo3DType type);
