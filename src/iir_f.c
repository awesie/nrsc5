#include <stdlib.h>

#include "iir_f.h"

#define WINDOW_SIZE 1024

void iir_f_init(iir_f_t *q, const iir_f_taps_t *taps, unsigned int npoles)
{
    q->taps = calloc(npoles + npoles + 1, sizeof(float));
    q->npoles = npoles;
    q->xwindow = calloc(WINDOW_SIZE, sizeof(float));
    q->ywindow = calloc(WINDOW_SIZE, sizeof(float));
    q->idx = npoles;
    q->gain = taps->gain;

    for (unsigned int i = 0; i < npoles; ++i)
        q->taps[i] = taps->ataps[i];

    for (unsigned int i = 0; i < npoles + 1; ++i)
        q->taps[npoles + i] = taps->btaps[i];
}

void iir_f_reset(iir_f_t *q)
{
    unsigned int npoles = q->npoles;

    for (unsigned int i = 0; i < npoles; i++)
    {
        q->xwindow[i] = 0;
        q->ywindow[i] = 0;
    }
    q->idx = npoles;
}

float iir_f_execute_generic(iir_f_t *q, float x)
{
    unsigned int npoles = q->npoles;
    float y = 0.0f;

    if (q->idx == WINDOW_SIZE)
    {
        for (unsigned int i = 0; i < npoles + 1; ++i)
        {
            q->xwindow[i] = q->xwindow[q->idx - npoles - 1 + i];
            q->ywindow[i] = q->ywindow[q->idx - npoles - 1 + i];
        }
        q->idx = npoles;
    }

    q->xwindow[q->idx] = x / q->gain;

    for (unsigned int i = 0; i < npoles + 1; i++)
        y += q->xwindow[q->idx - npoles + i] * q->taps[npoles + i];
    for (unsigned int i = 0; i < npoles; i++)
        y += q->ywindow[q->idx - npoles + i] * q->taps[i];

    q->ywindow[q->idx] = y;
    q->idx++;

    return y;
}
