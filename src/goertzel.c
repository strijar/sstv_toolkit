#include "internal/goertzel.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double         coefficient;
    double complex rotation;
    double complex s1;
    double complex s2;
} goertzel_t;

static goertzel_t goertzel_create(double frequency, double sample_rate) {
    double     w = 2.0 * M_PI * frequency / sample_rate;
    goertzel_t state = {
        .coefficient = 2.0 * cos(w),
        .rotation = cos(w) - I * sin(w),
        .s1 = 0.0,
        .s2 = 0.0,
    };
    return state;
}

static void goertzel_push(goertzel_t *state, double complex sample) {
    double complex s0 = sample + state->coefficient * state->s1 - state->s2;
    state->s2 = state->s1;
    state->s1 = s0;
}

static double goertzel_power(const goertzel_t *state) {
    double complex value = state->s1 - state->rotation * state->s2;
    double         real = creal(value);
    double         imag = cimag(value);
    return real * real + imag * imag;
}

void sstv_goertzel_pair_power(const float complex *samples, int count, double frequency0, double frequency1, double sample_rate, double *power0, double *power1, double *signal_energy) {
    goertzel_t tone0 = goertzel_create(frequency0, sample_rate);
    goertzel_t tone1 = goertzel_create(frequency1, sample_rate);
    double     energy = 0.0;

    for (int i = 0; i < count; ++i) {
        double complex sample = samples[i];
        goertzel_push(&tone0, sample);
        goertzel_push(&tone1, sample);
        if (signal_energy) {
            double real = creal(sample);
            double imag = cimag(sample);
            energy += real * real + imag * imag;
        }
    }

    if (power0)
        *power0 = goertzel_power(&tone0);
    if (power1)
        *power1 = goertzel_power(&tone1);
    if (signal_energy)
        *signal_energy = energy;
}
