#ifndef SSTV_INTERNAL_GOERTZEL_H
#define SSTV_INTERNAL_GOERTZEL_H

#include <complex.h>

/* Computes two tone powers and, optionally, window energy in one pass.
 * Frequencies are not quantized to DFT bins. */
void sstv_goertzel_pair_power(const float complex *samples, int count, double frequency0, double frequency1, double sample_rate, double *power0, double *power1, double *signal_energy);

#endif
