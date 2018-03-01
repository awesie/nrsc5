#pragma once

typedef struct {
    float *taps;
    unsigned int ntaps;
    float *window;
    unsigned int idx;
} fir_f_t;

void fir_f_init(fir_f_t *q, const float *taps, unsigned int ntaps);
void fir_f_reset(fir_f_t *q);
float fir_f_execute_halfband_15(fir_f_t *q, const float *x);
