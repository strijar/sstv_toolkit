#include "sstv/decoder.h"
#include "sstv/encoder.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    sstv_decoder_t *decoder;
    int             vis_count;
    int             line_count;
    int             complete_count;
} context_t;

typedef struct {
    int calls;
    int stop_at;
} cancel_context_t;

static void on_vis(void *user, const sstv_mode_t *mode) {
    context_t *context = user;
    (void) mode;
    context->vis_count++;
}

static void on_line(void *user, int row, int width, const uint8_t *rgb) {
    context_t *context = user;
    (void) row;
    (void) width;
    (void) rgb;
    context->line_count++;
}

static void on_frame(void *user, sstv_frame_status_t status, int rows) {
    context_t *context = user;
    (void) rows;
    if (status == SSTV_FRAME_COMPLETE)
        context->complete_count++;
}

static bool iq_sink(void *user, const float complex *samples, size_t count) {
    context_t *context = user;
    return sstv_decoder_push_iq(context->decoder, samples, count);
}

static bool cancelling_sink(void *user, const float complex *samples, size_t count) {
    cancel_context_t *context = user;
    (void) samples;
    (void) count;
    context->calls++;
    return context->calls < context->stop_at;
}

int main(void) {
    for (size_t index = 0; index < sstv_mode_count(); ++index) {
        const sstv_mode_t *mode = sstv_mode_at(index);
        size_t             stride = (size_t) mode->width * 3;
        size_t             size = stride * (size_t) mode->transmitted_lines;
        uint8_t           *image = malloc(size);
        if (!image)
            return 1;
        for (int y = 0; y < mode->transmitted_lines; ++y) {
            for (int x = 0; x < mode->width; ++x) {
                image[(size_t) y * stride + 3U * (size_t) x + 0] = (uint8_t) ((x * 255) / mode->width);
                image[(size_t) y * stride + 3U * (size_t) x + 1] = (uint8_t) ((y * 255) / mode->transmitted_lines);
                image[(size_t) y * stride + 3U * (size_t) x + 2] = (uint8_t) ((x + y) & 255);
            }
        }

        context_t              context = { 0 };
        const sstv_callbacks_t callbacks = { on_vis, on_line, on_frame };
        context.decoder = sstv_decoder_create(&callbacks, &context);
        bool ok = context.decoder && sstv_encode_rgb(mode, image, stride, iq_sink, &context);
        if (context.decoder)
            sstv_decoder_flush(context.decoder);
        ok = ok && context.vis_count == 1 && context.complete_count == 1 && context.line_count == sstv_mode_height(mode);
        if (!ok)
            fprintf(stderr, "%s failed: vis=%d lines=%d complete=%d\n", mode->short_name, context.vis_count, context.line_count, context.complete_count);
        sstv_decoder_destroy(context.decoder);

        cancel_context_t cancel = { .stop_at = 5 };
        bool             completed = sstv_encode_rgb(mode, image, stride, cancelling_sink, &cancel);
        ok = ok && !completed && cancel.calls == cancel.stop_at;
        if (completed || cancel.calls != cancel.stop_at)
            fprintf(stderr, "%s cancellation failed: result=%d calls=%d\n", mode->short_name, completed, cancel.calls);

        free(image);
        if (!ok)
            return 1;
    }
    return 0;
}
