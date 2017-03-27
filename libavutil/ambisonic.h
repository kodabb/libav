/*
 * Copyright (c) 2017 Vittorio Giovara <vittorio.giovara@gmail.com>
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
 * Ambisonic audio
 */

#ifndef AVUTIL_AMBISONIC_H
#define AVUTIL_AMBISONIC_H

#include <stddef.h>
#include <stdint.h>

/**
 * @addtogroup lavu_audio
 * @{
 *
 * @defgroup lavu_audio_ambisonic Ambisonic audio
 * @{
 */

/**
 * @addtogroup lavu_audio_ambisonic
 *
 * TODO
 *
 */

/**
 * Projection of the video surface(s) on a sphere.
 */
enum AVAmbisonicType {
    /**
     * Audio is not ambisonic.
     */
    AV_AMBISONIC_NON_DIEGETIC,

    /**
     * Audio is a full 3D ambisonic sound field.
     */
    AV_AMBISONIC_PERIPHONIC,
};

/**
 * A sound field is decomposed into spherical harmonic components (also known as
 * "degree"), defining zeroth, first, and higher order components.
 * These are collectively called B-Format.
 *
 * 0th-order ambisonic (1 channel) is a purely omni-directional signal
 *   containing no directional information.
 * 1st-order ambisonic (4 channels) contains directional information, with
 *   level for source accuracy.
 * 3rd-order ambisonic (16 channels) contains dramatically more directional
 *   information than 1st-order; sources can be localized with considerable
 *   accuracy.
 *
 * According to the following table, the soundfield describes a full sphere
 * only when the horizontal order matches the degree value. Degree can be
 * computed as `sqrt(n) - 1` where n is the number of channels.
 *
 * Number of | Degree | Height | Channels
 * channels  |        | order  | nomenclature
 * --------- | ------ | ------ | ----------------
 *     1     |   0    |   0    | W
 *     3     |   1    |   0    | WXY
 *     4     |   1    |   1    | WXYZ
 *     5     |   2    |   0    | WXYUV
 *     6     |   2    |   1    | WXYZUV
 *     9     |   2    |   2    | WXYZRSTUV
 *     7     |   3    |   0    | WXYUVPQ
 *     8     |   3    |   1    | WXYZUVPQ
 *    11     |   3    |   2    | WXYZRSTUVPQ
 *    16     |   3    |   3    | WXYZRSTUVKLMNOPQ
 */
enum AVAmbisonicChannelOrder {
    /**
     * Given a spherical harmonic of degree l and order m, the corresponding
     * ordering index n is given by n = l * (l + 1) + m.
     */
    AV_AMBISONIC_CHANNEL_ACN,
};

enum AVAmbisonicNormalization {
    /**
     * The normalization used is Schmidt semi-normalization (SN3D):
     * the spherical harmonic of degree l and order m is normalized according to
     *     sqrt((2 - δ(m)) * ((l - m)! / (l + m)!))
     * where δ(m) is the Kronecker delta function, such that
     *     δ(0) = 1 and δ(m) = 0 otherwise.
     */
    AV_AMBISONIC_NORM_SN3D,
};

/**
 * This structure describes an ambisonic soundfield, represented by spherical
 * harmonics coefficients using the associated Legendre polynomials (without
 * Condon-Shortley phase) as the basis functions. Thus, the spherical harmonic
 * of degree l and order m at elevation E and azimuth A is given by:
 *
 *     N(l, abs(m)) * P(l, abs(m), sin(E)) * T(m, A)
 *
 * where:
 * - N(l, m) is the spherical harmonics normalization function used.
 * - P(l, m, x) is the (unnormalized) associated Legendre polynomial, without
 *   Condon-Shortley phase, of degree l and order m evaluated at x.
 * - T(m, x) is sin(-m * x) for m < 0 and cos(m * x) otherwise.
 *
 * @note The struct must be allocated with av_ambisonic_alloc() and
 *       its size is not a part of the public ABI.
 */
typedef struct AVAmbisonic {
    /**
     * Ambisonic type.
     */
    enum AVAmbisonicType type;

    /**
     * The channel order system used to describe the ambisonic field.
     */
    enum AVAmbisonicChannelOrder order;

    /**
     * Spherical harmonics normalization used in the represented ambisonic audio.
     */
    enum AVAmbisonicNormalization normalization;

    /**
     * Number of audio channels contained in a given ambisonic audio track.
     * Non diegetic audio will have this field initialized to 0.
     */
    int nb_channels;

    /**
     * This field describes how the audio channels in a given audio track are
     * mapped to ambisonic components, given the defined channel ordering.
     * The sequence of channel_map values should match the channel sequence
     * within the given audio track.
     *
     * For example, consider a 4-channel audio track containing ambisonic
     * components W, X, Y, Z at channel indexes 0, 1, 2, 3, respectively.
     * For a AV_AMBISONIC_CHANNEL_ACN order, components should be ordered as
     * W, Y, Z, X, so the channel_map sequence should be 0, 2, 3, 1.
     */
    unsigned *channel_map;
} AVAmbisonic;

/**
 * Allocate a AVAmbisonic structure and initialize its fields to default
 * values. Memory allocated with this function needs to be freed with
 * av_ambisonic_free().
 *
 * @param size size of the structure
 * @return the newly allocated struct or NULL on failure
 */
AVAmbisonic *av_ambisonic_alloc(size_t *size, int nb_channels);

/**
 * Free a AVAmbisonic structure and all its allocated resources.
 *
 * @return the newly allocated struct or NULL on failure
 */
void av_ambisonic_free(AVAmbisonic **ambisonic);

/**
 * @}
 * @}
 */

#endif /* AVUTIL_AMBISONIC_H */
