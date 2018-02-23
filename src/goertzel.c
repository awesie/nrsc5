#include "goertzel.h"

void goertzel_init(goertzel_t *st, float frequency, float sample_rate, unsigned int N)
{
    unsigned int k = 0.5f + N * frequency / sample_rate;

    st->n = 0;
    st->N = N;
    st->q1 = 0.0f;
    st->q2 = 0.0f;
    st->coeff = 2 * cosf(2.0 * M_PI * k / N);
}

int goertzel_execute(goertzel_t *st, float sample, float *out)
{
    float q0 = st->coeff * st->q1 - st->q2 + sample;
    st->q2 = st->q1;
    st->q1 = q0;
    st->n++;

    if (st->n == st->N)
    {
        *out = st->q1 * st->q1 + st->q2 * st->q2 - st->q1 * st->q2 * st->coeff;
        st->q1 = 0.0;
        st->q2 = 0.0;
        st->n = 0;
        return 1;
    }
    else
    {
        return 0;
    }
}
