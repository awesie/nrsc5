#pragma once

#include "defines.h"

typedef struct
{
    float q1, q2, coeff;
    unsigned int n, N;
} goertzel_t;

void goertzel_init(goertzel_t *, float frequency, float sample_rate, unsigned int N);
int goertzel_execute(goertzel_t *, float sample, float *out);
