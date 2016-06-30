/*
 * Ratecontrol
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 * Copyright (c) 2002-2004 Michael Niedermayer
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

#ifndef AVCODEC_RATECONTROL_H
#define AVCODEC_RATECONTROL_H

/**
 * @file
 * ratecontrol header.
 */

#include <stdio.h>
#include <stdint.h>
#include "libavutil/eval.h"

#include "avcodec.h"

typedef struct Predictor{
    double coeff;
    double count;
    double decay;
} Predictor;

typedef struct RateControlEntry{
    int pict_type;
    float qscale;
    int mv_bits;
    int i_tex_bits;
    int p_tex_bits;
    int misc_bits;
    int header_bits;
    uint64_t expected_bits;
    int new_pict_type;
    float new_qscale;
    int mc_mb_var_sum;
    int mb_var_sum;
    int i_count;
    int skip_count;
    int f_code;
    int b_code;
}RateControlEntry;

#if !FF_API_PRIVATE_OPT_RC
typedef struct RcOverride {
    int start_frame;
    int end_frame;
    int qscale; // If this is 0 then quality_factor will be used instead.
    float quality_factor;
} RcOverride;
#endif

/**
 * rate control context.
 */
typedef struct RateControlContext{
    AVCodecContext *avctx;

    int num_entries;              ///< number of RateControlEntries
    RateControlEntry *entry;
    double buffer_index;          ///< amount of bits in the video/audio buffer
    Predictor pred[5];
    double short_term_qsum;       ///< sum of recent qscales
    double short_term_qcount;     ///< count of recent qscales
    double pass1_rc_eq_output_sum;///< sum of the output of the rc equation, this is used for normalization
    double pass1_wanted_bits;     ///< bits which should have been output by the pass1 code (including complexity init)
    double last_qscale;
    double last_qscale_for[5];    ///< last qscale for a specific pict type, used for max_diff & ipb factor stuff
    int last_mc_mb_var_sum;
    int last_mb_var_sum;
    uint64_t i_cplx_sum[5];
    uint64_t p_cplx_sum[5];
    uint64_t mv_bits_sum[5];
    uint64_t qscale_sum[5];
    int frame_count[5];
    int last_non_b_pict_type;

    void *non_lavc_opaque;        ///< context for non lavc rc code (for example xvid)
    float dry_run_qscale;         ///< for xvid rc
    int last_picture_number;      ///< for xvid rc
    AVExpr * rc_eq_eval;
    RcOverride *rc_override;
    int rc_override_count;
    int mb_num;
    enum AVPictureType pict_type;

    int bit_rate_tolerance;
    float qcompress;
    char *rc_overrides;
    char *rc_eq;
    int rc_qmod_freq;
    float rc_qmod_amp;
    int lmin, lmax;
    float rc_buffer_aggressivity;
    float rc_qsquish;
    float rc_initial_cplx;
    int rc_strategy;
}RateControlContext;

/* rate control */
int ff_rate_control_init(AVCodecContext *avctx, RateControlContext *rcc);
void ff_rate_control_uninit(RateControlContext *rcc);

float ff_rate_estimate_qscale(RateControlContext *rcc, int picture_number,
                              int dry_run);
int ff_vbv_update(RateControlContext *rcc, int frame_size);
void ff_get_2pass_fcode(RateControlContext *rcc, int entry,
                        int *f_code, int *b_code);

#endif /* AVCODEC_RATECONTROL_H */
