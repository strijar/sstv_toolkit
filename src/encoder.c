#include "sstv/encoder.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { OUTPUT_CAPACITY = 4096,
       MAX_WIDTH = 800 };

typedef struct {
    sstv_iq_sink_t sink;
    void          *user;
    float complex  output[OUTPUT_CAPACITY];
    size_t         output_count;
    double         phase;
    bool           ok;
} writer_t;

static uint8_t clamp8(double value) {
    if (value < 0.0)
        return 0;
    if (value > 255.0)
        return 255;
    return (uint8_t) lround(value);
}

static bool flush_output(writer_t *writer) {
    if (!writer->ok || writer->output_count == 0)
        return writer->ok;
    writer->ok = writer->sink(writer->user, writer->output, writer->output_count);
    writer->output_count = 0;
    return writer->ok;
}

static bool write_tone_samples(writer_t *writer, double frequency, long count) {
    double step = 2.0 * M_PI * frequency / SSTV_SAMPLE_RATE;
    for (long i = 0; i < count && writer->ok; ++i) {
        writer->output[writer->output_count++] = (float) cos(writer->phase) + I * (float) sin(writer->phase);
        writer->phase += step;
        if (writer->output_count == OUTPUT_CAPACITY)
            if (!flush_output(writer))
                return false;
    }
    writer->phase = fmod(writer->phase, 2.0 * M_PI);
    return writer->ok;
}

static bool write_tone(writer_t *writer, double frequency, double duration) {
    return write_tone_samples(writer, frequency, lround(duration * SSTV_SAMPLE_RATE));
}

static double level_frequency(uint8_t level) {
    return 1500.0 + (double) level * 800.0 / 255.0;
}

static bool write_channel(writer_t *writer, const uint8_t *levels, int width, double pixel_time) {
    double pixel_samples = pixel_time * SSTV_SAMPLE_RATE;
    double position = 0.0;
    for (int x = 0; x < width; ++x) {
        double next = position + pixel_samples;
        long   count = lround(next) - lround(position);
        if (!write_tone_samples(writer, level_frequency(levels[x]), count > 0 ? count : 1))
            return false;
        position = next;
    }
    return true;
}

static bool write_vis(writer_t *writer, int code) {
    int parity = 0;
    if (!write_tone(writer, 1900.0, 0.300) ||
        !write_tone(writer, 1200.0, 0.010) ||
        !write_tone(writer, 1900.0, 0.300) ||
        !write_tone(writer, 1200.0, 0.030))
        return false;
    for (int bit = 0; bit < 7; ++bit) {
        int value = (code >> bit) & 1;
        parity ^= value;
        if (!write_tone(writer, value ? 1100.0 : 1300.0, 0.030))
            return false;
    }
    return write_tone(writer, parity ? 1100.0 : 1300.0, 0.030) &&
           write_tone(writer, 1200.0, 0.030);
}

static const uint8_t *row_at(const uint8_t *rgb, size_t stride, int row) {
    return rgb + (size_t) row * stride;
}

static double robot_y(const uint8_t *pixel) {
    return 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2];
}

static void robot_chroma(double r, double g, double b, uint8_t *ry, uint8_t *by) {
    double y = 0.299 * r + 0.587 * g + 0.114 * b;
    *ry = clamp8(r - y + 128.0);
    *by = clamp8(b - y + 128.0);
}

static void pd_values(double r, double g, double b, uint8_t *y, uint8_t *cr, uint8_t *cb) {
    *y = clamp8(16.0 + (65.738 * r + 129.057 * g + 25.064 * b) / 256.0);
    *cb = clamp8(128.0 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256.0);
    *cr = clamp8(128.0 + (112.439 * r - 94.154 * g - 18.285 * b) / 256.0);
}

static bool encode_basic(writer_t *writer, const sstv_mode_t *mode, const uint8_t *rgb, size_t stride) {
    uint8_t channels[3][MAX_WIDTH];
    for (int y = 0; y < mode->transmitted_lines && writer->ok; ++y) {
        const uint8_t *row = row_at(rgb, stride, y);
        if (!write_tone(writer, 1200.0, mode->sync_time) ||
            (mode->porch_time > 0 && !write_tone(writer, 1500.0, mode->porch_time)))
            return false;
        int count = mode->encoding == SSTV_ENC_BW ? 1 : 3;
        for (int x = 0; x < mode->width; ++x) {
            const uint8_t *p = row + 3 * x;
            if (mode->encoding == SSTV_ENC_BW)
                channels[0][x] = clamp8(robot_y(p));
            else if (mode->encoding == SSTV_ENC_GBR) {
                channels[0][x] = p[1];
                channels[1][x] = p[2];
                channels[2][x] = p[0];
            } else {
                channels[0][x] = p[0];
                channels[1][x] = p[1];
                channels[2][x] = p[2];
            }
        }
        for (int channel = 0; channel < count; ++channel) {
            if (!write_channel(writer, channels[channel], mode->width, mode->pixel_time))
                return false;
            if (channel + 1 < count && mode->separator_time > 0)
                if (!write_tone(writer, 1500.0, mode->separator_time))
                    return false;
        }
    }
    return true;
}

static bool encode_scottie(writer_t *writer, const sstv_mode_t *mode, const uint8_t *rgb, size_t stride) {
    uint8_t channel[MAX_WIDTH];
    static const int order[] = { 1, 2, 0 }; /* green, blue, red */
    for (int y = 0; y < mode->transmitted_lines && writer->ok; ++y) {
        const uint8_t *row = row_at(rgb, stride, y);
        for (int part = 0; part < 3; ++part) {
            if (part == 2) {
                if (!write_tone(writer, 1200.0, mode->sync_time))
                    return false;
            }
            if (!write_tone(writer, 1500.0, mode->porch_time))
                return false;
            for (int x = 0; x < mode->width; ++x)
                channel[x] = row[3 * x + order[part]];
            if (!write_channel(writer, channel, mode->width, mode->pixel_time))
                return false;
        }
    }
    return true;
}

static bool encode_robot36(writer_t *writer, const sstv_mode_t *mode, const uint8_t *rgb, size_t stride) {
    uint8_t lum[MAX_WIDTH], ry[MAX_WIDTH / 2], by[MAX_WIDTH / 2];
    for (int y = 0; y < mode->transmitted_lines && writer->ok; y += 2) {
        int            y1 = y + 1 < mode->transmitted_lines ? y + 1 : y;
        const uint8_t *rows[2] = { row_at(rgb, stride, y), row_at(rgb, stride, y1) };
        for (int cx = 0; cx < mode->width / 2; ++cx) {
            double sum[3] = { 0 };
            for (int row = 0; row < 2; ++row)
                for (int dx = 0; dx < 2; ++dx)
                    for (int c = 0; c < 3; ++c)
                        sum[c] += rows[row][3 * (2 * cx + dx) + c];
            robot_chroma(sum[0] / 4, sum[1] / 4, sum[2] / 4, &ry[cx], &by[cx]);
        }
        for (int row = 0; row < 2 && y + row < mode->transmitted_lines; ++row) {
            for (int x = 0; x < mode->width; ++x)
                lum[x] = clamp8(robot_y(rows[row] + 3 * x));
            if (!write_tone(writer, 1200.0, mode->sync_time) ||
                !write_tone(writer, 1500.0, mode->porch_time) ||
                !write_channel(writer, lum, mode->width, mode->pixel_time) ||
                !write_tone(writer, row == 0 ? 1500.0 : 2300.0, mode->chroma_marker_time) ||
                !write_tone(writer, 1500.0, mode->chroma_porch_time) ||
                !write_channel(writer, row == 0 ? ry : by, mode->width / 2, mode->pixel_time))
                return false;
        }
    }
    return true;
}

static bool encode_robot422(writer_t *writer, const sstv_mode_t *mode, const uint8_t *rgb, size_t stride) {
    uint8_t lum[MAX_WIDTH], ry[MAX_WIDTH / 2], by[MAX_WIDTH / 2];
    for (int y = 0; y < mode->transmitted_lines && writer->ok; ++y) {
        const uint8_t *row = row_at(rgb, stride, y);
        for (int x = 0; x < mode->width; ++x)
            lum[x] = clamp8(robot_y(row + 3 * x));
        for (int cx = 0; cx < mode->width / 2; ++cx) {
            const uint8_t *a = row + 6 * cx, *b = a + 3;
            robot_chroma((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, (a[2] + b[2]) / 2.0, &ry[cx], &by[cx]);
        }
        if (!write_tone(writer, 1200.0, mode->sync_time) ||
            !write_channel(writer, lum, mode->width, mode->pixel_time) ||
            !write_tone(writer, 1500.0, mode->separator_time) ||
            !write_channel(writer, ry, mode->width / 2, mode->pixel_time) ||
            !write_tone(writer, 1500.0, mode->separator_time) ||
            !write_channel(writer, by, mode->width / 2, mode->pixel_time))
            return false;
    }
    return true;
}

static bool encode_pd(writer_t *writer, const sstv_mode_t *mode, const uint8_t *rgb, size_t stride) {
    uint8_t y0[MAX_WIDTH], y1[MAX_WIDTH], cr[MAX_WIDTH], cb[MAX_WIDTH];
    for (int y = 0; y < mode->transmitted_lines && writer->ok; y += 2) {
        int            second = y + 1 < mode->transmitted_lines ? y + 1 : y;
        const uint8_t *rows[2] = { row_at(rgb, stride, y), row_at(rgb, stride, second) };
        for (int x = 0; x < mode->width; ++x) {
            uint8_t unused_cr, unused_cb;
            pd_values(rows[0][3 * x], rows[0][3 * x + 1], rows[0][3 * x + 2], &y0[x], &unused_cr, &unused_cb);
            pd_values(rows[1][3 * x], rows[1][3 * x + 1], rows[1][3 * x + 2], &y1[x], &unused_cr, &unused_cb);
            uint8_t unused_y;
            pd_values((rows[0][3 * x] + rows[1][3 * x]) / 2.0, (rows[0][3 * x + 1] + rows[1][3 * x + 1]) / 2.0, (rows[0][3 * x + 2] + rows[1][3 * x + 2]) / 2.0, &unused_y, &cr[x], &cb[x]);
        }
        if (!write_tone(writer, 1200.0, mode->sync_time) ||
            !write_tone(writer, 1500.0, mode->porch_time) ||
            !write_channel(writer, y0, mode->width, mode->pixel_time) ||
            !write_channel(writer, cr, mode->width, mode->pixel_time) ||
            !write_channel(writer, cb, mode->width, mode->pixel_time) ||
            !write_channel(writer, y1, mode->width, mode->pixel_time))
            return false;
    }
    return true;
}

bool sstv_encode_rgb(const sstv_mode_t *mode, const uint8_t *rgb, size_t stride, sstv_iq_sink_t sink, void *user) {
    if (!mode || !rgb || !sink || mode->width <= 0 || mode->width > MAX_WIDTH || stride < (size_t) mode->width * 3)
        return false;
    writer_t writer = { .sink = sink, .user = user, .ok = true };
    if (!write_vis(&writer, mode->vis_code))
        return false;
    bool encoded;
    switch (mode->encoding) {
        case SSTV_ENC_GBR:
            if (mode->vis_code == 60 || mode->vis_code == 56 || mode->vis_code == 76) {
                encoded = encode_scottie(&writer, mode, rgb, stride);
                break;
            }
            encoded = encode_basic(&writer, mode, rgb, stride);
            break;
        case SSTV_ENC_RGB:
        case SSTV_ENC_BW:
            encoded = encode_basic(&writer, mode, rgb, stride);
            break;
        case SSTV_ENC_YUV_ROBOT:
            encoded = encode_robot36(&writer, mode, rgb, stride);
            break;
        case SSTV_ENC_YUV_ROBOT422:
            encoded = encode_robot422(&writer, mode, rgb, stride);
            break;
        case SSTV_ENC_YUV_PD:
            encoded = encode_pd(&writer, mode, rgb, stride);
            break;
        default:
            return false;
    }
    return encoded && flush_output(&writer);
}
