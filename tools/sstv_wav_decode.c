#define _POSIX_C_SOURCE 200809L

#include "sstv/decoder.h"

#include <errno.h>
#include <liquid/liquid.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_FRAMES = 4096, HILBERT_SEMILENGTH = 25 };

typedef struct {
    uint16_t format, channels;
    uint32_t sample_rate;
    uint16_t block_align, bits_per_sample;
    long data_offset;
    uint32_t data_size;
} wav_info_t;

typedef struct {
    uint8_t *pixels;
    int width, height, rows, frames, write_errors;
    const char *prefix;
} app_t;

static uint16_t le16(const uint8_t *p) {
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool read_exact(FILE *file, void *buffer, size_t size) {
    return fread(buffer, 1, size, file) == size;
}

static bool parse_wav(FILE *file, wav_info_t *info, char *error, size_t error_size) {
    uint8_t header[12];
    bool have_fmt = false, have_data = false;
    memset(info, 0, sizeof(*info));
    if (!read_exact(file, header, sizeof(header)) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        snprintf(error, error_size, "not a RIFF/WAVE file");
        return false;
    }
    while (!have_data) {
        uint8_t chunk[8];
        if (!read_exact(file, chunk, sizeof(chunk))) {
            snprintf(error, error_size, "missing WAV data chunk");
            return false;
        }
        uint32_t size = le32(chunk + 4);
        long payload = ftell(file);
        if (payload < 0) {
            snprintf(error, error_size, "cannot determine WAV position");
            return false;
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            size_t take = size < sizeof(fmt) ? size : sizeof(fmt);
            if (size < 16 || !read_exact(file, fmt, take)) {
                snprintf(error, error_size, "invalid WAV fmt chunk");
                return false;
            }
            info->format = le16(fmt);
            info->channels = le16(fmt + 2);
            info->sample_rate = le32(fmt + 4);
            info->block_align = le16(fmt + 12);
            info->bits_per_sample = le16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = payload;
            info->data_size = size;
            have_data = true;
        }
        if (!have_data && fseek(file, payload + (long) size + (long) (size & 1U), SEEK_SET) != 0) {
            snprintf(error, error_size, "invalid WAV chunk size");
            return false;
        }
    }
    if (!have_fmt) {
        snprintf(error, error_size, "WAV data precedes fmt chunk");
        return false;
    }
    unsigned int bytes = (unsigned int) ((info->bits_per_sample + 7U) / 8U);
    if (info->channels == 0 || info->sample_rate == 0 || bytes == 0 ||
        info->block_align < info->channels * bytes) {
        snprintf(error, error_size, "invalid WAV stream parameters");
        return false;
    }
    if (!((info->format == 1 && (info->bits_per_sample == 8 || info->bits_per_sample == 16 ||
                                info->bits_per_sample == 24 || info->bits_per_sample == 32)) ||
          (info->format == 3 && info->bits_per_sample == 32))) {
        snprintf(error, error_size, "unsupported WAV format %u/%u-bit", info->format, info->bits_per_sample);
        return false;
    }
    if (fseek(file, info->data_offset, SEEK_SET) != 0) {
        snprintf(error, error_size, "cannot seek to WAV audio data");
        return false;
    }
    return true;
}

static float decode_sample(const uint8_t *p, uint16_t format, uint16_t bits) {
    if (format == 3) {
        uint32_t word = le32(p);
        float value;
        memcpy(&value, &word, sizeof(value));
        return isfinite(value) ? value : 0.0f;
    }
    if (bits == 8)
        return ((float) p[0] - 128.0f) / 128.0f;
    if (bits == 16)
        return (float) (int16_t) le16(p) / 32768.0f;
    if (bits == 24) {
        int32_t value = (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
        if (value & 0x00800000)
            value |= (int32_t) 0xff000000;
        return (float) value / 8388608.0f;
    }
    return (float) (int32_t) le32(p) / 2147483648.0f;
}

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
    app->pixels = calloc((size_t) app->width * (size_t) app->height * 3U, 1);
    printf("VIS: %s (%dx%d, %s)\n", mode->name, app->width, app->height,
           sstv_encoding_name(mode->encoding));
}

static void on_line(void *user, int row, int width, const uint8_t *rgb) {
    app_t *app = user;
    if (!app->pixels || row < 0 || row >= app->height || width != app->width)
        return;
    memcpy(app->pixels + (size_t) row * (size_t) width * 3U, rgb, (size_t) width * 3U);
    if (app->rows < row + 1)
        app->rows = row + 1;
}

static bool write_ppm(const app_t *app, const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t bytes = (size_t) app->width * (size_t) app->height * 3U;
    bool ok = fprintf(file, "P6\n%d %d\n255\n", app->width, app->height) > 0 &&
              fwrite(app->pixels, 1, bytes, file) == bytes;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static void on_frame(void *user, sstv_frame_status_t status, int rows) {
    app_t *app = user;
    if (status == SSTV_FRAME_COMPLETE && app->pixels) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s_%03d.ppm", app->prefix, app->frames);
        if (n <= 0 || (size_t) n >= sizeof(path) || !write_ppm(app, path)) {
            fprintf(stderr, "cannot write decoded image\n");
            app->write_errors++;
        } else {
            printf("Frame #%d: %s\n", app->frames, path);
            app->frames++;
        }
    } else {
        fprintf(stderr, "%s frame after %d rows\n",
                status == SSTV_FRAME_TRUNCATED ? "Truncated" : "Aborted", rows);
    }
    clear_image(app);
}

static bool decode_audio(FILE *input, const wav_info_t *wav, sstv_decoder_t *decoder) {
    size_t bytes_per_sample = (size_t) ((wav->bits_per_sample + 7U) / 8U);
    size_t raw_capacity = (size_t) INPUT_FRAMES * wav->block_align;
    uint8_t *raw = malloc(raw_capacity);
    float complex *analytic = malloc((size_t) INPUT_FRAMES * sizeof(*analytic));
    float rate = (float) SSTV_SAMPLE_RATE / (float) wav->sample_rate;
    size_t out_capacity = (size_t) ceil(1.0 + 2.0 * rate * INPUT_FRAMES);
    float complex *resampled = malloc(out_capacity * sizeof(*resampled));
    firhilbf hilbert = firhilbf_create(HILBERT_SEMILENGTH, 60.0f);
    msresamp_crcf resampler = msresamp_crcf_create(rate, 60.0f);
    bool ok = raw && analytic && resampled && hilbert && resampler;
    uint32_t remaining = wav->data_size;
    while (ok && remaining >= wav->block_align) {
        size_t frames = remaining / wav->block_align;
        if (frames > INPUT_FRAMES)
            frames = INPUT_FRAMES;
        size_t bytes = frames * wav->block_align;
        if (fread(raw, 1, bytes, input) != bytes) {
            fprintf(stderr, "unexpected end of WAV audio data\n");
            ok = false;
            break;
        }
        remaining -= (uint32_t) bytes;
        for (size_t i = 0; i < frames; ++i) {
            float mono = 0.0f;
            const uint8_t *frame = raw + i * wav->block_align;
            for (uint16_t ch = 0; ch < wav->channels; ++ch)
                mono += decode_sample(frame + (size_t) ch * bytes_per_sample, wav->format, wav->bits_per_sample);
            mono /= (float) wav->channels;
            firhilbf_r2c_execute(hilbert, mono, &analytic[i]);
        }
        unsigned int produced = 0;
        msresamp_crcf_execute(resampler, analytic, (unsigned int) frames, resampled, &produced);
        if (produced > out_capacity || !sstv_decoder_push_iq(decoder, resampled, produced))
            ok = false;
    }
    if (ok && remaining != 0)
        fprintf(stderr, "warning: WAV data ends with a partial sample frame\n");
    if (hilbert)
        firhilbf_destroy(hilbert);
    if (resampler)
        msresamp_crcf_destroy(resampler);
    free(resampled);
    free(analytic);
    free(raw);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <input.wav> [output_prefix]\n", argv[0]);
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    wav_info_t wav;
    char error[160];
    if (!parse_wav(input, &wav, error, sizeof(error))) {
        fprintf(stderr, "%s: %s\n", argv[1], error);
        fclose(input);
        return 1;
    }
    printf("WAV: %u Hz, %u channel(s), %u-bit %s\n", wav.sample_rate, wav.channels,
           wav.bits_per_sample, wav.format == 3 ? "float" : "PCM");
    app_t app = { .prefix = argc == 3 ? argv[2] : "out" };
    const sstv_callbacks_t callbacks = { on_vis, on_line, on_frame };
    sstv_decoder_t *decoder = sstv_decoder_create(&callbacks, &app);
    bool ok = decoder && decode_audio(input, &wav, decoder);
    if (decoder && ok)
        sstv_decoder_flush(decoder);
    sstv_decoder_destroy(decoder);
    clear_image(&app);
    if (fclose(input) != 0)
        ok = false;
    printf("Complete frames: %d\n", app.frames);
    return ok && app.write_errors == 0 && app.frames > 0 ? 0 : 1;
}
