#include "fm_audio.h"
#include "private.h"

/*
 * GNU Radio Filter Design Tool
 * FIR, Low Pass, Kaiser Window
 * Sample rate: 1488375
 * End of pass band: 372094
 * Start of stop band: 530000
 * Stop band attenuation: 40
 */
static float decim_taps[] = {
    0.6062333583831787,
    -0.13481467962265015,
    0.032919470220804214,
    -0.00410953676328063
};

/*
 * http://www-users.cs.york.ac.uk/~fisher/mkfilter
 * IIR, Low Pass, Chebyshev
 * Ripple: -0.1
 * Order: 8
 * Sample Rate: 186047
 * Corner: 15000
 */
static const iir_f_taps_t fm_lpf_taps = {
    .gain = 1.670891391e6,
    .ataps = {
        -0.4271335192,
        3.5462797232,
        -13.1268451090,
        28.3001746570,
        -38.8810034930,
        34.8782976620,
        -19.9663956220,
        6.6764724893,
    },
    .btaps = {
        1,
        8,
        28,
        56,
        70,
        56,
        28,
        8,
        1
    }
};

/*
 * http://www-users.cs.york.ac.uk/~fisher/mkfilter
 * IIR, Band Stop, Chebyshev
 * Ripple: -0.1
 * Order: 2
 * Sample Rate: 186047
 * Corner: 15000, 23000
 */
static const iir_f_taps_t fm_bsf_taps = {
    .gain = 1.102869724f,
    .ataps = {
        -0.8235593684f,
        2.7895724264f,
        -4.1739342607f,
        3.0748641139f,
    },
    .btaps = {
        1.0f,
        -3.2338547532f,
        4.6144541412f,
        -3.2338547532f,
        1.0f
    }
};

/*
 * http://www-users.cs.york.ac.uk/~fisher/mkfilter
 * IIR, Low Pass, Butterworth
 * Order: 1
 * Sample Rate: 46512
 * Corner: 2122
 */
static const iir_f_taps_t fm_deemph_taps = {
    .gain = 7.929175225,
    .ataps = { 0.7477669564 },
    .btaps = { 1.0, 1.0 }
};

void fm_audio_push(fm_audio_t *st, const float input[2])
{
    float x[2], y;

    y = fir_f_execute_halfband_15(&st->bb_decim, input);
    y = iir_f_execute_generic(&st->pilot_bsf, y);
    y = iir_f_execute_generic(&st->mono_lpf, y);

    st->mono_predecim[st->mono_predecim_idx++] = y;
    if (st->mono_predecim_idx == 4)
    {
        unsigned int in_len = 1, out_len = 1;
        int16_t sample_in, sample_out;

        x[0] = fir_f_execute_halfband_15(&st->mono_decim[0], &st->mono_predecim[0]);
        x[1] = fir_f_execute_halfband_15(&st->mono_decim[0], &st->mono_predecim[2]);
        y = fir_f_execute_halfband_15(&st->mono_decim[1], x);
        y *= 10.0; // Amplify by 20 dB.
        y = iir_f_execute_generic(&st->deemph, y);
        sample_in = y * 32768.0;
        st->mono_predecim_idx = 0;

        speex_resampler_process_int(st->audio_resampler, 0, &sample_in, &in_len, &sample_out, &out_len);
        if (out_len)
        {
            st->samples[st->samples_idx++] = sample_out;
            st->samples[st->samples_idx++] = sample_out;
        }
    }

    if (st->samples_idx == 4096)
    {
        nrsc5_report_audio(st->radio, NRSC5_PROGRAM_ANALOG, st->samples, st->samples_idx);
        st->samples_idx = 0;
    }
}

void fm_audio_init(fm_audio_t *st, nrsc5_t *radio)
{
    int err;

    st->radio = radio;
    fir_f_init(&st->bb_decim, decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    fir_f_init(&st->mono_decim[0], decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    fir_f_init(&st->mono_decim[1], decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    st->mono_predecim_idx = 0;
    iir_f_init(&st->mono_lpf, &fm_lpf_taps, 8);
    iir_f_init(&st->pilot_bsf, &fm_bsf_taps, 4);
    iir_f_init(&st->deemph, &fm_deemph_taps, 1);
    st->samples = malloc(sizeof(int16_t) * 4096);
    st->samples_idx = 0;
    st->audio_resampler = nrsc5_resampler_init_frac(1, 135, 128, 46512, 44100, 1, &err);
}
