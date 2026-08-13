#include "sstv/decoder.h"
#include "sstv/iq_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int     vis;
    int     complete;
    int     truncated;
    int     lines;
    int     first_width;
    uint8_t first_row[800 * 3];
    uint8_t second_row[800 * 3];
} result_t;

static void on_vis(void *user, const sstv_mode_t *mode) {
    result_t *r = user;
    (void) mode;
    r->vis++;
}

static void on_line(void *user, int row, int width, const uint8_t *rgb) {
    result_t *r = user;
    if (width <= 800 && row == 0) {
        r->first_width = width;
        memcpy(r->first_row, rgb, (size_t) width * 3U);
    }
    if (width <= 800 && row == 1)
        memcpy(r->second_row, rgb, (size_t) width * 3U);
    r->lines++;
}

static void on_frame(void *user, sstv_frame_status_t status, int rows) {
    result_t *r = user;
    (void) rows;
    if (status == SSTV_FRAME_COMPLETE)
        r->complete++;
    if (status == SSTV_FRAME_TRUNCATED)
        r->truncated++;
}

static int feed_file(sstv_decoder_t *decoder, const char *path, size_t omit_samples) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long bytes = ftell(file);
    if (bytes < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    size_t remaining = (size_t) bytes / (sizeof(float) * 2U);
    if (omit_samples > remaining) {
        fclose(file);
        return 0;
    }
    remaining -= omit_samples;

    float complex samples[257];
    while (remaining) {
        size_t                capacity = remaining < 257U ? remaining : 257U;
        size_t                count = 0;
        sstv_iq_read_status_t status = sstv_iq_read(file, samples, capacity, &count);
        if (status == SSTV_IQ_READ_ERROR || status == SSTV_IQ_READ_TRUNCATED ||
            !sstv_decoder_push_iq(decoder, samples, count)) {
            fclose(file);
            return 0;
        }
        remaining -= count;
    }
    fclose(file);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    result_t               result = { 0 };
    const sstv_callbacks_t callbacks = { on_vis, on_line, on_frame };
    sstv_decoder_t        *decoder = sstv_decoder_create(&callbacks, &result);
    if (!decoder)
        return 1;

    int ok = 0;
    if (strcmp(argv[1], "double") == 0) {
        ok = feed_file(decoder, argv[2], 0U) && feed_file(decoder, argv[2], 0U);
        sstv_decoder_flush(decoder);
        ok = ok && result.vis == 2 && result.complete == 2 && result.truncated == 0;
    } else if (strcmp(argv[1], "truncated") == 0) {
        ok = feed_file(decoder, argv[2], 3000U);
        sstv_decoder_flush(decoder);
        ok = ok && result.vis == 1 && result.complete == 0 && result.truncated == 1;
    } else if (strcmp(argv[1], "first-line") == 0) {
        ok = feed_file(decoder, argv[2], 0U);
        sstv_decoder_flush(decoder);
        ok = ok && result.complete == 1 && result.first_width > 0 &&
             memcmp(result.first_row, result.second_row, (size_t) result.first_width * 3U) == 0;
    }

    if (!ok)
        fprintf(stderr, "vis=%d complete=%d truncated=%d lines=%d\n", result.vis, result.complete, result.truncated, result.lines);
    sstv_decoder_destroy(decoder);
    return ok ? 0 : 1;
}
