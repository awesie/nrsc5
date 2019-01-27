#pragma once

#include <stdint.h>
#include <stdio.h>
#include <complex.h>

#include <nrsc5.h>

#include "acquire.h"
#include "decode.h"
#include "defines.h"
#include "firdecim_q15.h"
#include "frame.h"
#include "output.h"
#include "sync.h"

#define INPUT_BUF_LEN (2160 * 512)
#define MAX_DECIM_LOG2 4

typedef int (*input_snr_cb_t) (void *, float);

typedef struct input_t
{
    nrsc5_t *radio;
    output_t *output;

    float complex phase, phase_increment;
    int decimation;
    int decim_log2;
    firdecim_q15 firdecim[MAX_DECIM_LOG2];
    cint16_t buffer[INPUT_BUF_LEN];
    unsigned int avail, used, skip;

    fftwf_plan snr_fft;
    float complex snr_fft_in[64];
    float complex snr_fft_out[64];
    float snr_power[64];
    int snr_cnt;
    input_snr_cb_t snr_cb;
    void *snr_cb_arg;

    acquire_t acq;
    decode_t decode;
    frame_t frame;
    sync_t sync;
} input_t;

void input_init(input_t *st, nrsc5_t *radio, output_t *output);
void input_free(input_t *st);
void input_cb(cint16_t *, uint32_t, void *);
int input_set_decimation(input_t *st, int);
void input_set_freq_offset(input_t *st, float offset);
void input_set_snr_callback(input_t *st, input_snr_cb_t cb, void *);
void input_set_skip(input_t *st, unsigned int skip);
void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len, unsigned int program);
void input_aas_push(input_t *st, uint8_t *psd, unsigned int len);
