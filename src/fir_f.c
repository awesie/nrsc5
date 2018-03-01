#include <stdlib.h>

#include "fir_f.h"

#define WINDOW_SIZE 1024

void fir_f_init(fir_f_t *q, const float *taps, unsigned int ntaps)
{
    q->taps = calloc(ntaps, sizeof(float));
    q->ntaps = ntaps;
    q->window = calloc(WINDOW_SIZE, sizeof(float));
    q->idx = ntaps - 1;

    // reverse order so we can push into the window
    for (unsigned int i = 0; i < ntaps; ++i)
        q->taps[i] = taps[ntaps - 1 - i];
}

void fir_f_reset(fir_f_t *q)
{
    q->idx = q->ntaps - 1;
}

static void push(fir_f_t *q, float x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (int i = 0; i < q->ntaps - 1; i++)
            q->window[i] = q->window[q->idx - q->ntaps + 1 + i];
        q->idx = q->ntaps - 1;
    }
    q->window[q->idx++] = x;
}

static float dotprod_halfband(fir_f_t *q, const float *a, const float *b)
{
    unsigned int ntaps = q->ntaps;
    float sum = 0;

    for (unsigned int i = 0; i < ntaps / 2; i += 2)
        sum += (a[i] + a[ntaps - 1 - i]) * b[i / 2];
    sum += a[ntaps / 2];

    return sum / 2.0;
}

float fir_f_execute_halfband_15(fir_f_t *q, const float *x)
{
    float y;

    push(q, x[0]);
    y = dotprod_halfband(q, &q->window[q->idx - q->ntaps], q->taps);
    push(q, x[1]);

    return y;
}
