#include "sstv/decoder.h"
#include "sstv/iq_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t    *pixels;
    int         width;
    int         height;
    int         rows;
    int         frame_count;
    const char *prefix;
} app_t;

static void clear_image(app_t *app) {
    free(app->pixels);
    app->pixels = NULL;
    app->width = app->height = app->rows = 0;
}

static void on_vis(void *user, const sstv_mode_t *mode) {
    app_t *app = user;
    clear_image(app);
    app->width = mode->width;
    app->height = sstv_mode_height(mode);
    app->pixels = calloc((size_t) app->width * (size_t) app->height * 3U, 1U);
    printf("VIS OK: %s (%dx%d, %s)\n", mode->name, app->width, app->height, sstv_encoding_name(mode->encoding));
}

static void on_line(void *user, int row, int width, const uint8_t *rgb) {
    app_t *app = user;
    if (!app->pixels || row < 0 || row >= app->height || width != app->width)
        return;
    memcpy(app->pixels + (size_t) row * (size_t) width * 3U, rgb, (size_t) width * 3U);
    if (row + 1 > app->rows)
        app->rows = row + 1;
}

static bool write_ppm(const app_t *app, const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t bytes = (size_t) app->width * (size_t) app->height * 3U;
    bool   ok = fprintf(file, "P6\n%d %d\n255\n", app->width, app->height) > 0 &&
              fwrite(app->pixels, 1U, bytes, file) == bytes;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static void on_frame(void *user, sstv_frame_status_t status, int rows) {
    app_t *app = user;
    if (status == SSTV_FRAME_COMPLETE && app->pixels) {
        char path[1024];
        int  n = snprintf(path, sizeof(path), "%s_%03d.ppm", app->prefix, app->frame_count);
        if (n > 0 && (size_t) n < sizeof(path) && write_ppm(app, path)) {
            printf("Frame #%d written: %s\n", app->frame_count, path);
            app->frame_count++;
        } else {
            fprintf(stderr, "Failed to write output PPM\n");
        }
    } else if (status == SSTV_FRAME_TRUNCATED) {
        fprintf(stderr, "Truncated stream: decoded %d rows\n", rows);
    } else if (status == SSTV_FRAME_ABORTED) {
        fprintf(stderr, "Frame aborted after %d rows\n", rows);
    }
    clear_image(app);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <raw_iq_f32.bin> [output_prefix]\n", argv[0]);
        return 2;
    }

    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    app_t                  app = { .prefix = argc >= 3 ? argv[2] : "out" };
    const sstv_callbacks_t callbacks = {
        .on_vis = on_vis,
        .on_line = on_line,
        .on_frame = on_frame,
    };
    sstv_decoder_t *decoder = sstv_decoder_create(&callbacks, &app);
    if (!decoder) {
        fprintf(stderr, "Failed to create SSTV decoder\n");
        fclose(input);
        return 1;
    }

    enum { CHUNK = 2048 };
    float complex samples[CHUNK];
    bool          ok = true;
    for (;;) {
        size_t                count = 0;
        sstv_iq_read_status_t status = sstv_iq_read(input, samples, CHUNK, &count);
        if (count && !sstv_decoder_push_iq(decoder, samples, count)) {
            fprintf(stderr, "Decoder error\n");
            ok = false;
            break;
        }
        if (status == SSTV_IQ_READ_EOF)
            break;
        if (status == SSTV_IQ_READ_TRUNCATED) {
            fprintf(stderr, "IQ file contains an incomplete I/Q pair\n");
            ok = false;
            break;
        }
        if (status == SSTV_IQ_READ_ERROR) {
            fprintf(stderr, "Failed to read IQ file\n");
            ok = false;
            break;
        }
    }

    if (ok)
        sstv_decoder_flush(decoder);
    printf("Completely decoded frames: %d\n", app.frame_count);
    clear_image(&app);
    sstv_decoder_destroy(decoder);
    if (fclose(input) != 0)
        ok = false;
    return ok ? 0 : 1;
}
