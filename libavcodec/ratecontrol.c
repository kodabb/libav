/*
 * Rate control for video encoders
 *
 * Copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
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
 * Rate control for video encoders.
 */

#include "libavutil/attributes.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"
#include "ratecontrol.h"
#include "mpegutils.h"
#include "mpegvideo.h"
#include "libavutil/eval.h"

#undef NDEBUG // Always check asserts, the speed effect is far too small to disable them.
#include <assert.h>

#ifndef M_E
#define M_E 2.718281828
#endif

static inline double qp2bits(RateControlEntry *rce, double qp)
{
    if (qp <= 0.0) {
        av_log(NULL, AV_LOG_ERROR, "qp<=0.0\n");
    }
    return rce->qscale * (double)(rce->i_tex_bits + rce->p_tex_bits + 1) / qp;
}

static inline double bits2qp(RateControlEntry *rce, double bits)
{
    if (bits < 0.9) {
        av_log(NULL, AV_LOG_ERROR, "bits<0.9\n");
    }
    return rce->qscale * (double)(rce->i_tex_bits + rce->p_tex_bits + 1) / bits;
}

static double get_diff_limited_q(RateControlContext *rcc,
                                 RateControlEntry *rce, double q)
{
    AVCodecContext *avctx     = rcc->avctx;
    const int pict_type       = rce->new_pict_type;
    const double last_p_q     = rcc->last_qscale_for[AV_PICTURE_TYPE_P];
    const double last_non_b_q = rcc->last_qscale_for[rcc->last_non_b_pict_type];

    if (pict_type == AV_PICTURE_TYPE_I &&
        (avctx->i_quant_factor > 0.0 || rcc->last_non_b_pict_type == AV_PICTURE_TYPE_P))
        q = last_p_q * FFABS(avctx->i_quant_factor) + avctx->i_quant_offset;
    else if (pict_type == AV_PICTURE_TYPE_B &&
             avctx->b_quant_factor > 0.0)
        q = last_non_b_q * avctx->b_quant_factor + avctx->b_quant_offset;
    if (q < 1)
        q = 1;

    /* last qscale / qdiff stuff */
    if (rcc->last_non_b_pict_type == pict_type || pict_type != AV_PICTURE_TYPE_I) {
        double last_q     = rcc->last_qscale_for[pict_type];
        const int maxdiff = FF_QP2LAMBDA * avctx->max_qdiff;

        if (q > last_q + maxdiff)
            q = last_q + maxdiff;
        else if (q < last_q - maxdiff)
            q = last_q - maxdiff;
    }

    rcc->last_qscale_for[pict_type] = q; // Note we cannot do that after blurring

    if (pict_type != AV_PICTURE_TYPE_B)
        rcc->last_non_b_pict_type = pict_type;

    return q;
}

/**
 * Get the qmin & qmax for pict_type.
 */
static void get_qminmax(RateControlContext *rcc, int pict_type,
                        int *qmin_ret, int *qmax_ret)
{
    int qmin = rcc->lmin;
    int qmax = rcc->lmax;

    assert(qmin <= qmax);

    switch (pict_type) {
    case AV_PICTURE_TYPE_B:
        qmin = (int)(qmin * FFABS(rcc->avctx->b_quant_factor) + rcc->avctx->b_quant_offset + 0.5);
        qmax = (int)(qmax * FFABS(rcc->avctx->b_quant_factor) + rcc->avctx->b_quant_offset + 0.5);
        break;
    case AV_PICTURE_TYPE_I:
        qmin = (int)(qmin * FFABS(rcc->avctx->i_quant_factor) + rcc->avctx->i_quant_offset + 0.5);
        qmax = (int)(qmax * FFABS(rcc->avctx->i_quant_factor) + rcc->avctx->i_quant_offset + 0.5);
        break;
    }

    qmin = av_clip(qmin, 1, FF_LAMBDA_MAX);
    qmax = av_clip(qmax, 1, FF_LAMBDA_MAX);

    if (qmax < qmin)
        qmax = qmin;

    *qmin_ret = qmin;
    *qmax_ret = qmax;
}

static double modify_qscale(RateControlContext *rcc, RateControlEntry *rce,
                            double q, int frame_num)
{
    AVCodecContext *avctx    = rcc->avctx;
    const double buffer_size = avctx->rc_buffer_size;
    const double fps         = 1 / av_q2d(avctx->time_base);
    const double min_rate    = avctx->rc_min_rate / fps;
    const double max_rate    = avctx->rc_max_rate / fps;
    const int pict_type      = rce->new_pict_type;
    int qmin, qmax;

    get_qminmax(rcc, pict_type, &qmin, &qmax);

    /* modulation */
    if (rcc->rc_qmod_freq &&
        frame_num % rcc->rc_qmod_freq == 0 &&
        pict_type == AV_PICTURE_TYPE_P)
        q *= rcc->rc_qmod_amp;

    /* buffer overflow/underflow protection */
    if (buffer_size) {
        double expected_size = rcc->buffer_index;
        double q_limit;

        if (min_rate) {
            double d = 2 * (buffer_size - expected_size) / buffer_size;
            if (d > 1.0)
                d = 1.0;
            else if (d < 0.0001)
                d = 0.0001;
            q *= pow(d, 1.0 / rcc->rc_buffer_aggressivity);

            q_limit = bits2qp(rce,
                              FFMAX((min_rate - buffer_size + rcc->buffer_index) *
                                    avctx->rc_min_vbv_overflow_use, 1));

            if (q > q_limit) {
                if (avctx->debug & FF_DEBUG_RC)
                    av_log(avctx, AV_LOG_DEBUG,
                           "limiting QP %f -> %f\n", q, q_limit);
                q = q_limit;
            }
        }

        if (max_rate) {
            double d = 2 * expected_size / buffer_size;
            if (d > 1.0)
                d = 1.0;
            else if (d < 0.0001)
                d = 0.0001;
            q /= pow(d, 1.0 / rcc->rc_buffer_aggressivity);

            q_limit = bits2qp(rce,
                              FFMAX(rcc->buffer_index *
                                    avctx->rc_max_available_vbv_use, 1));
            if (q < q_limit) {
                if (avctx->debug & FF_DEBUG_RC)
                    av_log(avctx, AV_LOG_DEBUG,
                           "limiting QP %f -> %f\n", q, q_limit);
                q = q_limit;
            }
        }
    }
    ff_dlog(avctx, "q:%f max:%f min:%f size:%f index:%f agr:%f\n",
            q, max_rate, min_rate, buffer_size, rcc->buffer_index,
            rcc->rc_buffer_aggressivity);
    if (rcc->rc_qsquish == 0.0 || qmin == qmax) {
        if (q < qmin)
            q = qmin;
        else if (q > qmax)
            q = qmax;
    } else {
        double min2 = log(qmin);
        double max2 = log(qmax);

        q  = log(q);
        q  = (q - min2) / (max2 - min2) - 0.5;
        q *= -4.0;
        q  = 1.0 / (1.0 + exp(q));
        q  = q * (max2 - min2) + min2;

        q = exp(q);
    }

    return q;
}

static int parse_overrides(RateControlContext *rcc)
{
    AVCodecContext *avctx = rcc->avctx;
    const char *p = rcc->rc_overrides;
    int i;

    for (i = 0; p; i++) {
        int start, end, q;
        int e = sscanf(p, "%d,%d,%d", &start, &end, &q);
        if (e != 3) {
            av_log(avctx, AV_LOG_ERROR, "error parsing rc_override\n");
            av_freep(&rcc->rc_override);
            return AVERROR_INVALIDDATA;
        }

        rcc->rc_override = av_realloc(rcc->rc_override,
                                      sizeof(RcOverride) * (i + 1));
        if (!rcc->rc_override) {
            av_log(avctx, AV_LOG_ERROR,
                   "Could not (re)allocate memory for rc_override.\n");
            av_freep(&rcc->rc_override);
            return AVERROR(ENOMEM);
        }

        rcc->rc_override[i].start_frame = start;
        rcc->rc_override[i].end_frame   = end;
        if (q > 0) {
            rcc->rc_override[i].qscale         = q;
            rcc->rc_override[i].quality_factor = 1.0;
        }
        else {
            rcc->rc_override[i].qscale         = 0;
            rcc->rc_override[i].quality_factor = -q / 100.0;
        }
        p = strchr(p, '/');
        if (p)
            p++;
    }
    rcc->rc_override_count = i;

    return 0;
}

/**
 * Modify the bitrate curve from pass1 for one frame.
 */
static double get_qscale(RateControlContext *rcc, RateControlEntry *rce,
                         double rate_factor, int frame_num)
{
    const int pict_type = rce->new_pict_type;
    const double mb_num = rcc->mb_num;
    double q, bits;
    int i, ret;

    double const_values[] = {
        M_PI,
        M_E,
        rce->i_tex_bits * rce->qscale,
        rce->p_tex_bits * rce->qscale,
        (rce->i_tex_bits + rce->p_tex_bits) * (double)rce->qscale,
        rce->mv_bits / mb_num,
        rce->pict_type == AV_PICTURE_TYPE_B ? (rce->f_code + rce->b_code) * 0.5 : rce->f_code,
        rce->i_count / mb_num,
        rce->mc_mb_var_sum / mb_num,
        rce->mb_var_sum / mb_num,
        rce->pict_type == AV_PICTURE_TYPE_I,
        rce->pict_type == AV_PICTURE_TYPE_P,
        rce->pict_type == AV_PICTURE_TYPE_B,
        rcc->qscale_sum[pict_type] / (double)rcc->frame_count[pict_type],
        rcc->qcompress,
        rcc->i_cplx_sum[AV_PICTURE_TYPE_I] / (double)rcc->frame_count[AV_PICTURE_TYPE_I],
        rcc->i_cplx_sum[AV_PICTURE_TYPE_P] / (double)rcc->frame_count[AV_PICTURE_TYPE_P],
        rcc->p_cplx_sum[AV_PICTURE_TYPE_P] / (double)rcc->frame_count[AV_PICTURE_TYPE_P],
        rcc->p_cplx_sum[AV_PICTURE_TYPE_B] / (double)rcc->frame_count[AV_PICTURE_TYPE_B],
        (rcc->i_cplx_sum[pict_type] + rcc->p_cplx_sum[pict_type]) / (double)rcc->frame_count[pict_type],
        0
    };

    bits = av_expr_eval(rcc->rc_eq_eval, const_values, rce);
    if (isnan(bits)) {
        av_log(rcc->avctx, AV_LOG_ERROR,
               "Error evaluating rc_eq \"%s\"\n", rcc->rc_eq);
        return -1;
    }

    rcc->pass1_rc_eq_output_sum += bits;
    bits *= rate_factor;
    if (bits < 0.0)
        bits = 0.0;
    bits += 1.0; // avoid 1/0 issues

    /* user override */
    ret = parse_overrides(rcc);
    if (ret < 0)
        return ret;

    for (i = 0; i < rcc->rc_override_count; i++) {
        RcOverride *rco = rcc->rc_override;
        if (rco[i].start_frame > frame_num)
            continue;
        if (rco[i].end_frame < frame_num)
            continue;

        if (rco[i].qscale)
            bits = qp2bits(rce, rco[i].qscale);  // FIXME move at end to really force it?
        else
            bits *= rco[i].quality_factor;
    }

#if FF_API_PRIVATE_OPT_RC
FF_DISABLE_DEPRECATION_WARNINGS
    for (i = 0; i < rcc->avctx->rc_override_count; i++) {
        RcOverride *rco = rcc->avctx->rc_override;
        if (rco[i].start_frame > frame_num)
            continue;
        if (rco[i].end_frame < frame_num)
            continue;

        if (rco[i].qscale)
            bits = qp2bits(rce, rco[i].qscale);  // FIXME move at end to really force it?
        else
            bits *= rco[i].quality_factor;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    q = bits2qp(rce, bits);

    /* I/B difference */
    if (pict_type == AV_PICTURE_TYPE_I && rcc->avctx->i_quant_factor < 0.0)
        q = -q * rcc->avctx->i_quant_factor + rcc->avctx->i_quant_offset;
    else if (pict_type == AV_PICTURE_TYPE_B && rcc->avctx->b_quant_factor < 0.0)
        q = -q * rcc->avctx->b_quant_factor + rcc->avctx->b_quant_offset;
    if (q < 1)
        q = 1;

    return q;
}

static int init_pass2(RateControlContext *rcc)
{
    AVCodecContext *avctx  = rcc->avctx;
    int i, toobig;
    double fps             = 1 / av_q2d(avctx->time_base);
    double complexity[5]   = { 0 }; // approximate bits at quant=1
    uint64_t const_bits[5] = { 0 }; // quantizer independent bits
    uint64_t all_const_bits;
    uint64_t all_available_bits = (uint64_t)(avctx->bit_rate *
                                             (double)rcc->num_entries / fps);
    double rate_factor          = 0;
    double step;
    const int filter_size = (int)(avctx->qblur * 4) | 1;
    double expected_bits;
    double *qscale, *blurred_qscale, qscale_sum;

    /* find complexity & const_bits & decide the pict_types */
    for (i = 0; i < rcc->num_entries; i++) {
        RateControlEntry *rce = &rcc->entry[i];

        rce->new_pict_type                = rce->pict_type;
        rcc->i_cplx_sum[rce->pict_type]  += rce->i_tex_bits * rce->qscale;
        rcc->p_cplx_sum[rce->pict_type]  += rce->p_tex_bits * rce->qscale;
        rcc->mv_bits_sum[rce->pict_type] += rce->mv_bits;
        rcc->frame_count[rce->pict_type]++;

        complexity[rce->new_pict_type] += (rce->i_tex_bits + rce->p_tex_bits) *
                                          (double)rce->qscale;
        const_bits[rce->new_pict_type] += rce->mv_bits + rce->misc_bits;
    }

    all_const_bits = const_bits[AV_PICTURE_TYPE_I] +
                     const_bits[AV_PICTURE_TYPE_P] +
                     const_bits[AV_PICTURE_TYPE_B];

    if (all_available_bits < all_const_bits) {
        av_log(avctx, AV_LOG_ERROR, "requested bitrate is too low\n");
        return -1;
    }

    qscale         = av_malloc(sizeof(double) * rcc->num_entries);
    blurred_qscale = av_malloc(sizeof(double) * rcc->num_entries);
    if (!qscale || !blurred_qscale) {
        av_free(qscale);
        av_free(blurred_qscale);
        return AVERROR(ENOMEM);
    }
    toobig = 0;

    for (step = 256 * 256; step > 0.0000001; step *= 0.5) {
        expected_bits = 0;
        rate_factor  += step;

        rcc->buffer_index = avctx->rc_buffer_size / 2;

        /* find qscale */
        for (i = 0; i < rcc->num_entries; i++) {
            RateControlEntry *rce = &rcc->entry[i];

            qscale[i] = get_qscale(rcc, &rcc->entry[i], rate_factor, i);
            rcc->last_qscale_for[rce->pict_type] = qscale[i];
        }
        assert(filter_size % 2 == 1);

        /* fixed I/B QP relative to P mode */
        for (i = rcc->num_entries - 1; i >= 0; i--) {
            RateControlEntry *rce = &rcc->entry[i];

            qscale[i] = get_diff_limited_q(rcc, rce, qscale[i]);
        }

        /* smooth curve */
        for (i = 0; i < rcc->num_entries; i++) {
            RateControlEntry *rce = &rcc->entry[i];
            const int pict_type   = rce->new_pict_type;
            int j;
            double q = 0.0, sum = 0.0;

            for (j = 0; j < filter_size; j++) {
                int index    = i + j - filter_size / 2;
                double d     = index - i;
                double coeff = avctx->qblur == 0 ? 1.0 : exp(-d * d / (avctx->qblur * avctx->qblur));

                if (index < 0 || index >= rcc->num_entries)
                    continue;
                if (pict_type != rcc->entry[index].new_pict_type)
                    continue;
                q   += qscale[index] * coeff;
                sum += coeff;
            }
            blurred_qscale[i] = q / sum;
        }

        /* find expected bits */
        for (i = 0; i < rcc->num_entries; i++) {
            RateControlEntry *rce = &rcc->entry[i];
            double bits;

            rce->new_qscale = modify_qscale(rcc, rce, blurred_qscale[i], i);

            bits  = qp2bits(rce, rce->new_qscale) + rce->mv_bits + rce->misc_bits;
            bits += 8 * ff_vbv_update(rcc, bits);

            rce->expected_bits = expected_bits;
            expected_bits     += bits;
        }

        ff_dlog(rcc->avctx,
                "expected_bits: %f all_available_bits: %d rate_factor: %f\n",
                expected_bits, (int)all_available_bits, rate_factor);
        if (expected_bits > all_available_bits) {
            rate_factor -= step;
            ++toobig;
        }
    }
    av_free(qscale);
    av_free(blurred_qscale);

    /* check bitrate calculations and print info */
    qscale_sum = 0.0;
    for (i = 0; i < rcc->num_entries; i++) {
        ff_dlog(avctx, "[lavc rc] entry[%d].new_qscale = %.3f  qp = %.3f\n",
                i,
                rcc->entry[i].new_qscale,
                rcc->entry[i].new_qscale / FF_QP2LAMBDA);
        qscale_sum += av_clip(rcc->entry[i].new_qscale / FF_QP2LAMBDA,
                              avctx->qmin, avctx->qmax);
    }
    assert(toobig <= 40);
    av_log(avctx, AV_LOG_DEBUG,
           "[lavc rc] requested bitrate: %d bps  expected bitrate: %d bps\n",
           avctx->bit_rate,
           (int)(expected_bits / ((double)all_available_bits / avctx->bit_rate)));
    av_log(avctx, AV_LOG_DEBUG,
           "[lavc rc] estimated target average qp: %.3f\n",
           (float)qscale_sum / rcc->num_entries);
    if (toobig == 0) {
        av_log(avctx, AV_LOG_INFO,
               "[lavc rc] Using all of requested bitrate is not "
               "necessary for this video with these parameters.\n");
    } else if (toobig == 40) {
        av_log(avctx, AV_LOG_ERROR,
               "[lavc rc] Error: bitrate too low for this video "
               "with these parameters.\n");
        return -1;
    } else if (fabs(expected_bits / all_available_bits - 1.0) > 0.01) {
        av_log(avctx, AV_LOG_ERROR,
               "[lavc rc] Error: 2pass curve failed to converge\n");
        return -1;
    }

    return 0;
}

av_cold int ff_rate_control_init(AVCodecContext *avctx, RateControlContext *rcc)
{
    int i, res;
    static const char * const const_names[] = {
        "PI",
        "E",
        "iTex",
        "pTex",
        "tex",
        "mv",
        "fCode",
        "iCount",
        "mcVar",
        "var",
        "isI",
        "isP",
        "isB",
        "avgQP",
        "qComp",
        "avgIITex",
        "avgPITex",
        "avgPPTex",
        "avgBPTex",
        "avgTex",
        NULL
    };
    static double (* const func1[])(void *, double) = {
        (double (*)(void *, double)) bits2qp,
        (double (*)(void *, double)) qp2bits,
        NULL
    };
    static const char * const func1_names[] = {
        "bits2qp",
        "qp2bits",
        NULL
    };
    emms_c();

    rcc->avctx = avctx;

    res = av_expr_parse(&rcc->rc_eq_eval,
                        rcc->rc_eq ? rcc->rc_eq : "tex^qComp",
                        const_names, func1_names, func1,
                        NULL, NULL, 0, avctx);
    if (res < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error parsing rc_eq \"%s\"\n", rcc->rc_eq);
        return res;
    }

    for (i = 0; i < 5; i++) {
        rcc->pred[i].coeff = FF_QP2LAMBDA * 7.0;
        rcc->pred[i].count = 1.0;
        rcc->pred[i].decay = 0.4;

        rcc->i_cplx_sum [i] =
        rcc->p_cplx_sum [i] =
        rcc->mv_bits_sum[i] =
        rcc->qscale_sum [i] =
        rcc->frame_count[i] = 1; // 1 is better because of 1/0 and such

        rcc->last_qscale_for[i] = FF_QP2LAMBDA * 5;
    }
    rcc->buffer_index = avctx->rc_initial_buffer_occupancy;

    if (avctx->flags & AV_CODEC_FLAG_PASS2) {
        int i;
        char *p;

        /* find number of pics */
        p = avctx->stats_in;
        for (i = -1; p; i++)
            p = strchr(p + 1, ';');
        i += avctx->max_b_frames;
        if (i <= 0 || i >= INT_MAX / sizeof(RateControlEntry))
            return -1;
        rcc->entry       = av_mallocz(i * sizeof(RateControlEntry));
        rcc->num_entries = i;
        if (!rcc->entry)
            return AVERROR(ENOMEM);

        /* init all to skipped P-frames
         * (with B-frames we might have a not encoded frame at the end FIXME) */
        for (i = 0; i < rcc->num_entries; i++) {
            RateControlEntry *rce = &rcc->entry[i];

            rce->pict_type  = rce->new_pict_type = AV_PICTURE_TYPE_P;
            rce->qscale     = rce->new_qscale    = FF_QP2LAMBDA * 2;
            rce->misc_bits  = rcc->mb_num + 10;
            rce->mb_var_sum = rcc->mb_num * 100;
        }

        /* read stats */
        p = avctx->stats_in;
        for (i = 0; i < rcc->num_entries - avctx->max_b_frames; i++) {
            RateControlEntry *rce;
            int picture_number;
            int e;
            char *next;

            next = strchr(p, ';');
            if (next) {
                (*next) = 0; // sscanf is unbelievably slow on looong strings // FIXME copy / do not write
                next++;
            }
            e = sscanf(p, " in:%d ", &picture_number);

            assert(picture_number >= 0);
            assert(picture_number < rcc->num_entries);
            rce = &rcc->entry[picture_number];

            e += sscanf(p, " in:%*d out:%*d type:%d q:%f itex:%d ptex:%d mv:%d misc:%d fcode:%d bcode:%d mc-var:%d var:%d icount:%d skipcount:%d hbits:%d",
                        &rce->pict_type, &rce->qscale, &rce->i_tex_bits, &rce->p_tex_bits,
                        &rce->mv_bits, &rce->misc_bits,
                        &rce->f_code, &rce->b_code,
                        &rce->mc_mb_var_sum, &rce->mb_var_sum,
                        &rce->i_count, &rce->skip_count, &rce->header_bits);
            if (e != 14) {
                av_log(avctx, AV_LOG_ERROR,
                       "statistics are damaged at line %d, parser out=%d\n",
                       i, e);
                return -1;
            }

            p = next;
        }

        if (init_pass2(rcc) < 0) {
            ff_rate_control_uninit(rcc);
            return -1;
        }
    }

    if (!(avctx->flags & AV_CODEC_FLAG_PASS2)) {
        rcc->short_term_qsum   = 0.001;
        rcc->short_term_qcount = 0.001;

        rcc->pass1_rc_eq_output_sum = 0.001;
        rcc->pass1_wanted_bits      = 0.001;

        if (avctx->qblur > 1.0) {
            av_log(avctx, AV_LOG_ERROR, "qblur too large\n");
            return -1;
        }
        /* init stuff with the user specified complexity */
        if (rcc->rc_initial_cplx) {
            for (i = 0; i < 60 * 30; i++) {
                double bits = rcc->rc_initial_cplx * (i / 10000.0 + 1.0) * rcc->mb_num;
                RateControlEntry rce;

                if (i % ((avctx->gop_size + 3) / 4) == 0)
                    rce.pict_type = AV_PICTURE_TYPE_I;
                else if (i % (avctx->max_b_frames + 1))
                    rce.pict_type = AV_PICTURE_TYPE_B;
                else
                    rce.pict_type = AV_PICTURE_TYPE_P;

                rce.new_pict_type = rce.pict_type;
                rce.mc_mb_var_sum = bits * rcc->mb_num / 100000;
                rce.mb_var_sum    = rcc->mb_num;

                rce.qscale    = FF_QP2LAMBDA * 2;
                rce.f_code    = 2;
                rce.b_code    = 1;
                rce.misc_bits = 1;

                if (rcc->pict_type == AV_PICTURE_TYPE_I) {
                    rce.i_count    = rcc->mb_num;
                    rce.i_tex_bits = bits;
                    rce.p_tex_bits = 0;
                    rce.mv_bits    = 0;
                } else {
                    rce.i_count    = 0; // FIXME we do know this approx
                    rce.i_tex_bits = 0;
                    rce.p_tex_bits = bits * 0.9;
                    rce.mv_bits    = bits * 0.1;
                }
                rcc->i_cplx_sum[rce.pict_type]  += rce.i_tex_bits * rce.qscale;
                rcc->p_cplx_sum[rce.pict_type]  += rce.p_tex_bits * rce.qscale;
                rcc->mv_bits_sum[rce.pict_type] += rce.mv_bits;
                rcc->frame_count[rce.pict_type]++;

                get_qscale(rcc, &rce, rcc->pass1_wanted_bits / rcc->pass1_rc_eq_output_sum, i);

                // FIXME misbehaves a little for variable fps
                rcc->pass1_wanted_bits += avctx->bit_rate / (1 / av_q2d(avctx->time_base));
            }
        }
    }

    return 0;
}

av_cold void ff_rate_control_uninit(RateControlContext *rcc)
{
    emms_c();

    av_expr_free(rcc->rc_eq_eval);
    av_freep(&rcc->entry);
    av_freep(&rcc->rc_override);
}

int ff_vbv_update(RateControlContext *rcc, int frame_size)
{
    AVCodecContext *avctx = rcc->avctx;
    const double fps      = 1 / av_q2d(avctx->time_base);
    const int buffer_size = avctx->rc_buffer_size;
    const double min_rate = avctx->rc_min_rate / fps;
    const double max_rate = avctx->rc_max_rate / fps;

    ff_dlog(avctx, "%d %f %d %f %f\n",
            buffer_size, rcc->buffer_index, frame_size, min_rate, max_rate);

    if (buffer_size) {
        int left;

        rcc->buffer_index -= frame_size;
        if (rcc->buffer_index < 0) {
            av_log(avctx, AV_LOG_ERROR, "rc buffer underflow\n");
            rcc->buffer_index = 0;
        }

        left = buffer_size - rcc->buffer_index - 1;
        rcc->buffer_index += av_clip(left, min_rate, max_rate);

        if (rcc->buffer_index > buffer_size) {
            int stuffing = ceil((rcc->buffer_index - buffer_size) / 8);

            if (stuffing < 4 && avctx->codec_id == AV_CODEC_ID_MPEG4)
                stuffing = 4;
            rcc->buffer_index -= 8 * stuffing;

            if (avctx->debug & FF_DEBUG_RC)
                av_log(avctx, AV_LOG_DEBUG, "stuffing %d bytes\n", stuffing);

            return stuffing;
        }
    }
    return 0;
}

static double predict_size(Predictor *p, double q, double var)
{
    return p->coeff * var / (q * p->count);
}

static void update_predictor(Predictor *p, double q, double var, double size)
{
    double new_coeff = size * q / (var + 1);
    if (var < 10)
        return;

    p->count *= p->decay;
    p->coeff *= p->decay;
    p->count++;
    p->coeff += new_coeff;
}

static void adaptive_quantization(RateControlContext *rcc, Picture *pic,
                                  double q)
{
    AVCodecContext *avctx = rcc->avctx;
    MpegEncContext *s = avctx->priv_data;
    int i;
    const float lumi_masking         = s->avctx->lumi_masking / (128.0 * 128.0);
    const float dark_masking         = s->avctx->dark_masking / (128.0 * 128.0);
    const float temp_cplx_masking    = s->avctx->temporal_cplx_masking;
    const float spatial_cplx_masking = s->avctx->spatial_cplx_masking;
    const float p_masking            = s->avctx->p_masking;
    const float border_masking       = s->border_masking;
    float bits_sum                   = 0.0;
    float cplx_sum                   = 0.0;
    float *cplx_tab                  = s->cplx_tab;
    float *bits_tab                  = s->bits_tab;
    const int qmin                   = s->avctx->mb_lmin;
    const int qmax                   = s->avctx->mb_lmax;
    const int mb_width               = s->mb_width;
    const int mb_height              = s->mb_height;

    for (i = 0; i < rcc->mb_num; i++) {
        const int mb_xy = s->mb_index2xy[i];
        float temp_cplx = sqrt(pic->mc_mb_var[mb_xy]); // FIXME merge in pow()
        float spat_cplx = sqrt(pic->mb_var[mb_xy]);
        const int lumi  = pic->mb_mean[mb_xy];
        float bits, cplx, factor;
        int mb_x = mb_xy % s->mb_stride;
        int mb_y = mb_xy / s->mb_stride;
        int mb_distance;
        float mb_factor = 0.0;
        if (spat_cplx < 4)
            spat_cplx = 4;              // FIXME fine-tune
        if (temp_cplx < 4)
            temp_cplx = 4;              // FIXME fine-tune

        if ((s->mb_type[mb_xy] & CANDIDATE_MB_TYPE_INTRA)) { // FIXME hq mode
            cplx   = spat_cplx;
            factor = 1.0 + p_masking;
        } else {
            cplx   = temp_cplx;
            factor = pow(temp_cplx, -temp_cplx_masking);
        }
        factor *= pow(spat_cplx, -spatial_cplx_masking);

        if (lumi > 127)
            factor *= (1.0 - (lumi - 128) * (lumi - 128) * lumi_masking);
        else
            factor *= (1.0 - (lumi - 128) * (lumi - 128) * dark_masking);

        if (mb_x < mb_width / 5) {
            mb_distance = mb_width / 5 - mb_x;
            mb_factor   = (float)mb_distance / (float)(mb_width / 5);
        } else if (mb_x > 4 * mb_width / 5) {
            mb_distance = mb_x - 4 * mb_width / 5;
            mb_factor   = (float)mb_distance / (float)(mb_width / 5);
        }
        if (mb_y < mb_height / 5) {
            mb_distance = mb_height / 5 - mb_y;
            mb_factor   = FFMAX(mb_factor,
                                (float)mb_distance / (float)(mb_height / 5));
        } else if (mb_y > 4 * mb_height / 5) {
            mb_distance = mb_y - 4 * mb_height / 5;
            mb_factor   = FFMAX(mb_factor,
                                (float)mb_distance / (float)(mb_height / 5));
        }

        factor *= 1.0 - border_masking * mb_factor;

        if (factor < 0.00001)
            factor = 0.00001;

        bits        = cplx * factor;
        cplx_sum   += cplx;
        bits_sum   += bits;
        cplx_tab[i] = cplx;
        bits_tab[i] = bits;
    }

    /* handle qmin/qmax clipping */
    if (s->mpv_flags & FF_MPV_FLAG_NAQ) {
        float factor = bits_sum / cplx_sum;
        for (i = 0; i < rcc->mb_num; i++) {
            float newq = q * cplx_tab[i] / bits_tab[i];
            newq *= factor;

            if (newq > qmax) {
                bits_sum -= bits_tab[i];
                cplx_sum -= cplx_tab[i] * q / qmax;
            } else if (newq < qmin) {
                bits_sum -= bits_tab[i];
                cplx_sum -= cplx_tab[i] * q / qmin;
            }
        }
        if (bits_sum < 0.001)
            bits_sum = 0.001;
        if (cplx_sum < 0.001)
            cplx_sum = 0.001;
    }

    for (i = 0; i < rcc->mb_num; i++) {
        const int mb_xy = s->mb_index2xy[i];
        float newq      = q * cplx_tab[i] / bits_tab[i];
        int intq;

        if (s->mpv_flags & FF_MPV_FLAG_NAQ) {
            newq *= bits_sum / cplx_sum;
        }

        intq = (int)(newq + 0.5);

        if (intq > qmax)
            intq = qmax;
        else if (intq < qmin)
            intq = qmin;
        s->lambda_table[mb_xy] = intq;
    }
}

void ff_get_2pass_fcode(RateControlContext *rcc, int entry,
                        int *f_code, int *b_code)
{
    RateControlEntry *rce = &rcc->entry[entry];

    *f_code = rce->f_code;
    *b_code = rce->b_code;
}

// FIXME rd or at least approx for dquant

float ff_rate_estimate_qscale(RateControlContext *rcc, Picture *pic,
                              Picture *dts_pic, int picture_number,
                              enum AVPictureType last_pict_type, int dry_run)
{
    AVCodecContext *avctx = rcc->avctx;
    MpegEncContext *s = avctx->priv_data;
    float q;
    int qmin, qmax;
    float br_compensation;
    double diff;
    double short_term_q;
    double fps;
    int64_t wanted_bits;
    RateControlEntry local_rce, *rce;
    double bits;
    double rate_factor;
    int var;
    const int pict_type = s->pict_type;
    emms_c();

    get_qminmax(rcc, pict_type, &qmin, &qmax);

    fps = 1 / av_q2d(avctx->time_base);
    /* update predictors */
    if (picture_number > 2 && !dry_run) {
        const int last_var = last_pict_type == AV_PICTURE_TYPE_I ? rcc->last_mb_var_sum
                                                                 : rcc->last_mc_mb_var_sum;
        update_predictor(&rcc->pred[last_pict_type],
                         rcc->last_qscale,
                         sqrt(last_var), s->frame_bits);
    }

    if (avctx->flags & AV_CODEC_FLAG_PASS2) {
        assert(picture_number >= 0);
        assert(picture_number < rcc->num_entries);
        rce         = &rcc->entry[picture_number];
        wanted_bits = rce->expected_bits;
    } else {
        rce = &local_rce;

        if (!dts_pic || dts_pic->f->pts == AV_NOPTS_VALUE)
            wanted_bits = (uint64_t)(avctx->bit_rate * (double)picture_number / fps);
        else
            wanted_bits = (uint64_t)(avctx->bit_rate * (double)dts_pic->f->pts / fps);
    }

    diff = s->total_bits - wanted_bits;
    br_compensation = (rcc->bit_rate_tolerance - diff) / rcc->bit_rate_tolerance;
    if (br_compensation <= 0.0)
        br_compensation = 0.001;

    var = pict_type == AV_PICTURE_TYPE_I ? pic->mb_var_sum : pic->mc_mb_var_sum;

    short_term_q = 0; /* avoid warning */
    if (avctx->flags & AV_CODEC_FLAG_PASS2) {
        if (pict_type != AV_PICTURE_TYPE_I)
            assert(pict_type == rce->new_pict_type);

        q = rce->new_qscale / br_compensation;
        ff_dlog(avctx, "%f %f %f last:%d var:%d type:%d//\n", q,
                rce->new_qscale, br_compensation, s->frame_bits, var, pict_type);
    } else {
        rce->pict_type     =
        rce->new_pict_type = pict_type;
        rce->mc_mb_var_sum = pic->mc_mb_var_sum;
        rce->mb_var_sum    = pic->mb_var_sum;
        rce->qscale        = FF_QP2LAMBDA * 2;
        rce->f_code        = s->f_code;
        rce->b_code        = s->b_code;
        rce->misc_bits     = 1;

        bits = predict_size(&rcc->pred[pict_type], rce->qscale, sqrt(var));
        if (pict_type == AV_PICTURE_TYPE_I) {
            rce->i_count    = rcc->mb_num;
            rce->i_tex_bits = bits;
            rce->p_tex_bits = 0;
            rce->mv_bits    = 0;
        } else {
            rce->i_count    = 0;    // FIXME we do know this approx
            rce->i_tex_bits = 0;
            rce->p_tex_bits = bits * 0.9;
            rce->mv_bits    = bits * 0.1;
        }
        rcc->i_cplx_sum[pict_type]  += rce->i_tex_bits * rce->qscale;
        rcc->p_cplx_sum[pict_type]  += rce->p_tex_bits * rce->qscale;
        rcc->mv_bits_sum[pict_type] += rce->mv_bits;
        rcc->frame_count[pict_type]++;

        bits        = rce->i_tex_bits + rce->p_tex_bits;
        rate_factor = rcc->pass1_wanted_bits /
                      rcc->pass1_rc_eq_output_sum * br_compensation;

        q = get_qscale(rcc, rce, rate_factor, picture_number);
        if (q < 0)
            return -1;

        assert(q > 0.0);
        q = get_diff_limited_q(rcc, rce, q);
        assert(q > 0.0);

        // FIXME type dependent blur like in 2-pass
        if (pict_type == AV_PICTURE_TYPE_P || s->intra_only) {
            rcc->short_term_qsum   *= avctx->qblur;
            rcc->short_term_qcount *= avctx->qblur;

            rcc->short_term_qsum += q;
            rcc->short_term_qcount++;
            q = short_term_q = rcc->short_term_qsum / rcc->short_term_qcount;
        }
        assert(q > 0.0);

        q = modify_qscale(rcc, rce, q, picture_number);

        rcc->pass1_wanted_bits += avctx->bit_rate / fps;

        assert(q > 0.0);
    }

    if (avctx->debug & FF_DEBUG_RC) {
        av_log(avctx, AV_LOG_DEBUG,
               "%c qp:%d<%2.1f<%d %d want:%d total:%d comp:%f st_q:%2.2f "
               "size:%d var:%d/%d br:%d fps:%d\n",
               av_get_picture_type_char(pict_type),
               qmin, q, qmax, picture_number,
               (int)wanted_bits / 1000, (int)s->total_bits / 1000,
               br_compensation, short_term_q, s->frame_bits,
               pic->mb_var_sum, pic->mc_mb_var_sum,
               avctx->bit_rate / 1000, (int)fps);
    }

    if (q < qmin)
        q = qmin;
    else if (q > qmax)
        q = qmax;

    if (s->adaptive_quant)
        adaptive_quantization(rcc, pic, q);
    else
        q = (int)(q + 0.5);

    if (!dry_run) {
        rcc->last_qscale        = q;
        rcc->last_mc_mb_var_sum = pic->mc_mb_var_sum;
        rcc->last_mb_var_sum    = pic->mb_var_sum;
    }
    return q;
}
