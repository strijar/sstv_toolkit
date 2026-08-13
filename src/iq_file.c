#include "sstv/iq_file.h"

#include <stdlib.h>

sstv_iq_read_status_t sstv_iq_read(FILE *file, float complex *samples, size_t capacity, size_t *count) {
    if (!file || !samples || !count || capacity == 0)
        return SSTV_IQ_READ_ERROR;

    float *raw = malloc(capacity * 2U * sizeof(*raw));
    if (!raw)
        return SSTV_IQ_READ_ERROR;

    size_t n = fread(raw, sizeof(*raw), capacity * 2U, file);
    size_t pairs = n / 2U;
    for (size_t i = 0; i < pairs; ++i)
        samples[i] = raw[2U * i] + I * raw[2U * i + 1U];
    free(raw);
    *count = pairs;

    if (n % 2U != 0U)
        return SSTV_IQ_READ_TRUNCATED;
    if (ferror(file))
        return SSTV_IQ_READ_ERROR;
    return n == 0U ? SSTV_IQ_READ_EOF : SSTV_IQ_READ_OK;
}
