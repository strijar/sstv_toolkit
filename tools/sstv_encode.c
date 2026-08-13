#include "sstv/encoder.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE  *file;
    bool   ok;
    size_t samples;
} output_t;

static bool read_token(FILE *file, char *token, size_t capacity) {
    int c;
    do {
        c = fgetc(file);
        if (c == '#')
            while (c != '\n' && c != EOF)
                c = fgetc(file);
    } while (isspace(c));
    if (c == EOF)
        return false;
    size_t length = 0;
    while (c != EOF && !isspace(c)) {
        if (length + 1 >= capacity)
            return false;
        token[length++] = (char) c;
        c = fgetc(file);
    }
    token[length] = '\0';
    return true;
}

static uint8_t *read_ppm(const char *path, int *width, int *height) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    char token[64];
    bool valid = read_token(file, token, sizeof(token)) && strcmp(token, "P6") == 0;
    valid = valid && read_token(file, token, sizeof(token));
    if (valid)
        *width = atoi(token);
    valid = valid && read_token(file, token, sizeof(token));
    if (valid)
        *height = atoi(token);
    valid = valid && read_token(file, token, sizeof(token)) && atoi(token) == 255;
    size_t   size = valid && *width > 0 && *height > 0 ? (size_t) *width * (size_t) *height * 3 : 0;
    uint8_t *pixels = size ? malloc(size) : NULL;
    if (!pixels || fread(pixels, 1, size, file) != size) {
        free(pixels);
        pixels = NULL;
    }
    fclose(file);
    return pixels;
}

static bool write_iq(void *user, const float complex *samples, size_t count) {
    output_t *output = user;
    float     raw[8192];
    if (count > 4096)
        return false;
    for (size_t i = 0; i < count; ++i) {
        raw[2 * i] = crealf(samples[i]);
        raw[2 * i + 1] = cimagf(samples[i]);
    }
    output->ok = fwrite(raw, sizeof(float), 2 * count, output->file) == 2 * count;
    output->samples += output->ok ? count : 0;
    return output->ok;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <mode> <input.ppm> <output.iq>\nModes:", argv[0]);
        for (size_t i = 0; i < sstv_mode_count(); ++i)
            fprintf(stderr, " %s", sstv_mode_at(i)->short_name);
        fputc('\n', stderr);
        return 2;
    }
    const sstv_mode_t *mode = sstv_mode_find(argv[1]);
    int                width = 0, height = 0;
    uint8_t           *pixels = mode ? read_ppm(argv[2], &width, &height) : NULL;
    if (!mode || !pixels || width != mode->width || height != mode->transmitted_lines) {
        fprintf(stderr, "Unknown mode, invalid PPM, or dimensions do not match the mode\n");
        free(pixels);
        return 1;
    }
    FILE *file = fopen(argv[3], "wb");
    if (!file) {
        fprintf(stderr, "%s: %s\n", argv[3], strerror(errno));
        free(pixels);
        return 1;
    }
    output_t output = { .file = file, .ok = true };
    bool     ok = sstv_encode_rgb(mode, pixels, (size_t) width * 3, write_iq, &output);
    if (fclose(file) != 0)
        ok = false;
    free(pixels);
    if (!ok) {
        fprintf(stderr, "Encoding failed\n");
        return 1;
    }
    printf("Wrote %zu IQ samples (%.2f s)\n", output.samples, (double) output.samples / SSTV_SAMPLE_RATE);
    return 0;
}
