#ifndef SSTV_IQ_FILE_H
#define SSTV_IQ_FILE_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    SSTV_IQ_READ_OK,
    SSTV_IQ_READ_EOF,
    SSTV_IQ_READ_TRUNCATED,
    SSTV_IQ_READ_ERROR
} sstv_iq_read_status_t;

sstv_iq_read_status_t sstv_iq_read(FILE *file, float complex *samples, size_t capacity, size_t *count);

#endif
