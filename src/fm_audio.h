#pragma once

#include <nrsc5.h>

#include "config.h"
#include "fir_f.h"
#include "iir_f.h"
#include "resampler/speex_resampler.h"

typedef struct
{
    nrsc5_t *radio;

    fir_f_t bb_decim;
    fir_f_t mono_decim[2];
    float mono_predecim[4];
    unsigned int mono_predecim_idx;
    iir_f_t mono_lpf;
    iir_f_t pilot_bsf;
    iir_f_t deemph;
    int16_t *samples;
    unsigned int samples_idx;
    SpeexResamplerState *audio_resampler;
} fm_audio_t;

void fm_audio_init(fm_audio_t *st, nrsc5_t *radio);
void fm_audio_push(fm_audio_t *st, const float x[2]);
