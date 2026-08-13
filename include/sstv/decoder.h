#ifndef SSTV_DECODER_H
#define SSTV_DECODER_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sstv/modes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSTV_FRAME_COMPLETE,
    SSTV_FRAME_TRUNCATED,
    SSTV_FRAME_ABORTED
} sstv_frame_status_t;

typedef struct {
    void (*on_vis)(void *user, const sstv_mode_t *mode);
    void (*on_line)(void *user, int row, int width, const uint8_t *rgb);
    void (*on_frame)(void *user, sstv_frame_status_t status, int decoded_rows);
} sstv_callbacks_t;

typedef struct sstv_decoder sstv_decoder_t;

sstv_decoder_t *sstv_decoder_create(const sstv_callbacks_t *callbacks, void *user);
void            sstv_decoder_destroy(sstv_decoder_t *decoder);

/* Processes samples synchronously and invokes callbacks before returning. */
bool sstv_decoder_push_iq(sstv_decoder_t *decoder, const float complex *samples, size_t count);

/* Finishes the input stream. An incomplete line is never emitted as complete. */
void sstv_decoder_flush(sstv_decoder_t *decoder);

/* Discards the current frame and starts searching for the next VIS header. */
void sstv_decoder_reset(sstv_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
