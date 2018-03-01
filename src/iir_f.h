#pragma once

typedef struct {
    float gain;
    const float ataps[32];
    const float btaps[32];
} iir_f_taps_t;

typedef struct {
    float *taps;
    unsigned int npoles;
    float *xwindow, *ywindow;
    unsigned int idx;
    float gain;
} iir_f_t;

void iir_f_init(iir_f_t *q, const iir_f_taps_t *taps, unsigned int npoles);
void iir_f_reset(iir_f_t *q);
float iir_f_execute_generic(iir_f_t *q, float x);
