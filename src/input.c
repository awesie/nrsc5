/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "defines.h"
#include "input.h"
#include "private.h"

#define INPUT_BUF_LEN (2160 * 512)
#define FM_PILOT_LIMIT 1000.0
#define FM_DEMOD_DAMPING (sqrtf(2.0f) / 2.0f)
#define FM_DEMOD_LOOP_BW (1.0f / 20.0f)

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

static float input_fm_demod(input_t *st, cint16_t x)
{
    const float max_freq = 2 * M_PI * 90000 / (SAMPLE_RATE / 2);
    const float damping = FM_DEMOD_DAMPING;
    const float loop_bw = 2 * M_PI * FM_DEMOD_LOOP_BW;
    const float alpha = 4 * damping * loop_bw / (1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw);
    const float beta = 4 * loop_bw * loop_bw / (1.0f + 2.0f * damping * loop_bw + loop_bw * loop_bw);
    float y;

#if 1
    y = st->fm_demod_freq / (M_PI / 2);

    float error = cargf(cq15_to_cf(x)) - st->fm_demod_phase;

    if (error > M_PI)
        error -= 2 * M_PI;
    if (error < -M_PI)
        error += 2 * M_PI;

    st->fm_demod_freq += beta * error;
    st->fm_demod_phase += st->fm_demod_freq + alpha * error;

    while (st->fm_demod_phase > 2 * M_PI)
        st->fm_demod_phase -= 2 * M_PI;
    while (st->fm_demod_phase < -2 * M_PI)
        st->fm_demod_phase += 2 * M_PI;

    if (st->fm_demod_freq > max_freq)
        st->fm_demod_freq = max_freq;
    else if (st->fm_demod_freq < -max_freq)
        st->fm_demod_freq = -max_freq;

#else
    // Assuming that compiler is "normal", limit input to [-1, 0, 1].
    x.r >>= 15;
    x.i >>= 15;

    // Demodulator based on goo.gl/JH9VJo.
    // We scale range from [-2, 2] -> [-1, 1].
    y = 0.5f * ((x.r - st->fm_demod_q0.r) * st->fm_demod_q1.i - (x.i - st->fm_demod_q0.i) * st->fm_demod_q1.r);

    st->fm_demod_q0 = st->fm_demod_q1;
    st->fm_demod_q1 = x;
#endif

    return y;
}

static void input_push_to_acquire(input_t *st)
{
    if (st->skip)
    {
        if (st->skip > st->avail - st->used)
        {
            st->skip -= st->avail - st->used;
            st->used = st->avail;
        }
        else
        {
            st->used += st->skip;
            st->skip = 0;
        }
    }

    st->used += acquire_push(&st->acq, &st->buffer[st->used], st->avail - st->used);
}

void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len, unsigned int program, int gain)
{
    output_push(st->output, pdu, len, program, gain);
}

void input_set_skip(input_t *st, unsigned int skip)
{
    st->skip += skip;
}

static void measure_snr(input_t *st, cint16_t *buf, uint32_t len)
{
    unsigned int i, j;

    // use a small FFT to calculate magnitude of frequency ranges
    for (j = 64; j <= len; j += 64)
    {
        for (i = 0; i < 64; i++)
        {
            st->snr_fft_in[i] = cq15_to_cf(buf[i + j - 64]) * pow(sinf(M_PI*i/63), 2);
        }
        fftwf_execute(st->snr_fft);
        fftshift(st->snr_fft_out, 64);

        for (i = 0; i < 64; i++)
            st->snr_power[i] += normf(st->snr_fft_out[i]);
        st->snr_cnt++;
    }

    for (i = 0; i < len; i += 4)
    {
        cint16_t x[2], y[2], z;
        float angle, mag, mag2;

        x[0] = buf[i];
        x[1] = buf[i + 1];
        halfband_q15_execute(st->firdecim[0], x, &y[0]);
        x[0] = buf[i + 2];
        x[1] = buf[i + 3];
        halfband_q15_execute(st->firdecim[0], x, &y[1]);
        halfband_q15_execute(st->fm_firdecim, y, &z);

        angle = input_fm_demod(st, z);
        if (goertzel_execute(&st->fm_pilot, angle / M_PI, &mag))
        {
            mag = fminf(FM_PILOT_LIMIT, mag);
            st->fm_pilot_sum += mag*mag;
            st->fm_pilot_idx++;
        }
        if (goertzel_execute(&st->fm_not_pilot, angle / M_PI, &mag2))
        {
            mag2 = fminf(FM_PILOT_LIMIT, mag2);
            mag2 *= 16;
            st->fm_not_pilot_sum += mag2*mag2;
            st->fm_not_pilot_idx++;
        }
    }

    if (st->snr_cnt >= SNR_FFT_COUNT)
    {
        // noise bands are the frequncies near our signal
        float noise_lo = 0;
        for (i = 19; i < 23; i++)
            noise_lo += st->snr_power[i];
        noise_lo /= 4;
        float noise_hi = 0;
        for (i = 41; i < 45; i++)
            noise_hi += st->snr_power[i];
        noise_hi /= 4;
        // signal bands are the frequencies in our signal
        float signal_lo = (st->snr_power[24] + st->snr_power[25]) / 2;
        float signal_hi = (st->snr_power[39] + st->snr_power[40]) / 2;

        #if 0
        float snr_lo = noise_lo == 0 ? 0 : signal_lo / noise_lo;
        float snr_hi = noise_hi == 0 ? 0 : signal_hi / noise_hi;
        log_debug("%f %f (SNR: %f) %f %f (SNR: %f)", signal_lo, noise_lo, snr_lo, signal_hi, noise_hi, snr_hi);
        #endif

        float signal = (signal_lo + signal_hi) / 2 / st->snr_cnt;
        float noise = (noise_lo + noise_hi) / 2 / st->snr_cnt;
        float snr = signal / noise;

        float pilot_avg = st->fm_pilot_sum / st->fm_pilot_idx;
        float not_pilot_avg = (st->fm_not_pilot_sum / st->fm_not_pilot_idx);

        if (st->snr_cb(st->snr_cb_arg, snr, 10 * log10f(pilot_avg / not_pilot_avg)) == 0)
            st->snr_cb = NULL;

        st->snr_cnt = 0;
        for (i = 0; i < 64; ++i)
            st->snr_power[i] = 0;

        st->fm_pilot_idx = 0;
        st->fm_pilot_sum = 0;
        st->fm_not_pilot_idx = 0;
        st->fm_not_pilot_sum = 0;
    }
}

void input_cb(cint16_t *buf, uint32_t len, void *arg)
{
    unsigned int i, j, new_avail;
    input_t *st = arg;

    // Avoid clipping by immediately decreasing gain.
    // None of the supported drivers have better than 14-bit resolution,
    // so this will not lose any information.
    for (i = 0; i < len; i++)
    {
        buf[i].r /= 2;
        buf[i].i /= 2;
    }

    for (j = 1; j < st->decim_log2; j++)
    {
        for (i = 0; i < len; i += 2)
        {
            cint16_t x[2];

            x[0].r = buf[i].r;
            x[0].i = buf[i].i;
            x[1].r = buf[i + 1].r;
            x[1].i = buf[i + 1].i;

            halfband_q15_execute(st->firdecim[j], x, &buf[i / 2]);
        }
        len /= 2;
    }

    if (st->offset_tuning)
    {
        // Correct frequency offset
        for (i = 0; i < len; i++)
        {
            st->phase = cq15_mul(st->phase, st->phase_increment);
            buf[i] = cq15_mul(buf[i], st->phase);

            // Prevent error accumulation by resetting after one cycle.
            if (++st->phase_idx == (unsigned int)(SAMPLE_RATE * 2 / FREQ_OFFSET_FACTOR))
            {
                st->phase_idx = 0;
                st->phase = st->phase_increment;
            }
        }
    }

    if (st->snr_cb)
    {
        measure_snr(st, buf, len);
        return;
    }

    nrsc5_report_iq(st->radio, buf, len * sizeof(buf[0]));

    if (len / 2 + st->avail > INPUT_BUF_LEN)
    {
        if (st->avail > st->used)
        {
            memmove(&st->buffer[0], &st->buffer[st->used], (st->avail - st->used) * sizeof(st->buffer[0]));
            st->avail -= st->used;
            st->used = 0;
        }
        else
        {
            st->avail = 0;
            st->used = 0;
        }
    }
    new_avail = st->avail;

    if (len / 2 + new_avail > INPUT_BUF_LEN)
    {
        log_error("input buffer overflow!");
        return;
    }

    for (i = 0; i < len; i += 2)
    {
        cint16_t y;

        halfband_q15_execute(st->firdecim[0], &buf[i], &y);
        st->buffer[new_avail++] = (cint16_t){ y.r, -y.i };
    }

    static FILE *fmout;
    if (fmout == NULL)
        fmout = fopen("/tmp/fm.raw", "wb");

    for (i = st->avail; i < new_avail; i += 4)
    {
        float x[2];
        cint16_t z;

        halfband_q15_execute(st->fm_firdecim, &st->buffer[i], &z);
        x[0] = input_fm_demod(st, z);

        halfband_q15_execute(st->fm_firdecim, &st->buffer[i + 2], &z);
        x[1] = input_fm_demod(st, z);

        fm_audio_push(&st->fm_audio, x);
    }

    st->avail = new_avail;
    while (st->avail - st->used >= FFTCP)
    {
        input_push_to_acquire(st);
        acquire_process(&st->acq);
    }
}

void input_set_snr_callback(input_t *st, input_snr_cb_t cb, void *arg)
{
    st->snr_cb = cb;
    st->snr_cb_arg = arg;
}

void input_reset(input_t *st)
{
    st->avail = 0;
    st->used = 0;
    st->skip = 0;
    for (int i = 0; i < 64; ++i)
        st->snr_power[i] = 0;
    st->snr_cnt = 0;

    st->phase = st->phase_increment;
    st->phase_idx = 0;
    for (int i = 0; i < MAX_DECIM_LOG2; i++)
        firdecim_q15_reset(st->firdecim[i]);

    firdecim_q15_reset(st->fm_firdecim);
    st->fm_demod_phase = 0;
    st->fm_demod_freq = 0;
    goertzel_init(&st->fm_pilot, 19000.0f, SAMPLE_RATE / 2, 372 * 4);
    goertzel_init(&st->fm_not_pilot, 17000.0f, SAMPLE_RATE / 2, 372 / 4);
    st->fm_pilot_sum = 0;
    st->fm_pilot_idx = 0;
    st->fm_not_pilot_sum = 0;
    st->fm_not_pilot_idx = 0;

    acquire_reset(&st->acq);
    decode_reset(&st->decode);
    frame_reset(&st->frame);
    sync_reset(&st->sync);
}

int input_set_decimation(input_t *st, int decimation)
{
    if (decimation == 2)
        st->decim_log2 = 1;
    else if (decimation == 4)
        st->decim_log2 = 2;
    else if (decimation == 8)
        st->decim_log2 = 3;
    else if (decimation == 16)
        st->decim_log2 = 4;
    else
        return 1;
    st->decimation = decimation;
    return 0;
}

void input_set_offset_tuning(input_t *st, int enabled)
{
    st->offset_tuning = enabled;
}

void input_init(input_t *st, nrsc5_t *radio, output_t *output)
{
    st->buffer = malloc(sizeof(cint16_t) * INPUT_BUF_LEN);
    st->radio = radio;
    st->output = output;
    st->snr_cb = NULL;
    st->snr_cb_arg = NULL;

    st->offset_tuning = 1;
    st->phase_increment = cf_to_cq15(cexpf(2 * M_PI * FREQ_OFFSET / (SAMPLE_RATE * 2) * I));

    st->decimation = 2;
    st->decim_log2 = 1;

    for (int i = 0; i < MAX_DECIM_LOG2; i++)
        st->firdecim[i] = firdecim_q15_create(decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));
    st->snr_fft = fftwf_plan_dft_1d(64, st->snr_fft_in, st->snr_fft_out, FFTW_FORWARD, 0);
    st->fm_firdecim = firdecim_q15_create(decim_taps, sizeof(decim_taps) / sizeof(decim_taps[0]));

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    sync_init(&st->sync, st);
    fm_audio_init(&st->fm_audio, st->radio);

    input_reset(st);
}

void input_aas_push(input_t *st, uint8_t *psd, unsigned int len)
{
    output_aas_push(st->output, psd, len);
}
