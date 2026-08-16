#include "sstv/encoder.h"
#include <hackrf.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { TX_SAMPLE_RATE = 2000000, RING_SIZE = 4 * 1024 * 1024 };

typedef struct {
    uint8_t data[RING_SIZE];
    size_t read_pos, write_pos, used;
    bool producer_done, failed, padded_last_block;
    pthread_mutex_t mutex;
    pthread_cond_t can_read, can_write;
} ring_t;

typedef struct {
    ring_t *ring;
    float complex previous;
    uint64_t input_index, output_index;
    bool have_previous;
} resampler_t;

typedef struct {
    const sstv_mode_t *mode;
    const uint8_t *pixels;
    size_t stride;
    ring_t *ring;
} producer_t;

static volatile sig_atomic_t interrupted;
static void on_signal(int number) { (void) number; interrupted = 1; }

static bool read_token(FILE *file, char *token, size_t capacity) {
    int c;
    do {
        c = fgetc(file);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(file);
    } while (c != EOF && isspace((unsigned char) c));
    if (c == EOF) return false;
    size_t length = 0;
    while (c != EOF && !isspace((unsigned char) c)) {
        if (length + 1 >= capacity) return false;
        token[length++] = (char) c;
        c = fgetc(file);
    }
    token[length] = '\0';
    return true;
}

static bool parse_dimension(const char *text, int *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed <= 0 || parsed > 1000000) return false;
    *value = (int) parsed;
    return true;
}

static uint8_t *read_ppm(const char *path, int *width, int *height) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char token[64];
    bool valid = read_token(file, token, sizeof(token)) && strcmp(token, "P6") == 0;
    valid = valid && read_token(file, token, sizeof(token)) && parse_dimension(token, width);
    valid = valid && read_token(file, token, sizeof(token)) && parse_dimension(token, height);
    valid = valid && read_token(file, token, sizeof(token)) && strcmp(token, "255") == 0;
    size_t size = valid && (size_t) *width <= SIZE_MAX / (size_t) *height / 3
                      ? (size_t) *width * (size_t) *height * 3 : 0;
    uint8_t *pixels = size ? malloc(size) : NULL;
    if (!pixels || fread(pixels, 1, size, file) != size) { free(pixels); pixels = NULL; }
    fclose(file);
    return pixels;
}

static bool ring_write(ring_t *ring, const uint8_t *data, size_t count) {
    while (count) {
        pthread_mutex_lock(&ring->mutex);
        while (ring->used == RING_SIZE && !interrupted && !ring->failed)
            pthread_cond_wait(&ring->can_write, &ring->mutex);
        if (interrupted || ring->failed) { pthread_mutex_unlock(&ring->mutex); return false; }
        size_t part = count < RING_SIZE - ring->used ? count : RING_SIZE - ring->used;
        if (part > RING_SIZE - ring->write_pos) part = RING_SIZE - ring->write_pos;
        memcpy(ring->data + ring->write_pos, data, part);
        ring->write_pos = (ring->write_pos + part) % RING_SIZE;
        ring->used += part;
        pthread_cond_signal(&ring->can_read);
        pthread_mutex_unlock(&ring->mutex);
        data += part;
        count -= part;
    }
    return true;
}

static int8_t iq_byte(float value) {
    long scaled = lroundf(value * 127.0f);
    if (scaled < -127) scaled = -127;
    if (scaled > 127) scaled = 127;
    return (int8_t) scaled;
}

static bool emit_iq(resampler_t *r, float complex sample) {
    uint8_t bytes[2] = { (uint8_t) iq_byte(crealf(sample)), (uint8_t) iq_byte(cimagf(sample)) };
    return ring_write(r->ring, bytes, sizeof(bytes));
}

static bool resample_iq(void *user, const float complex *samples, size_t count) {
    resampler_t *r = user;
    for (size_t i = 0; i < count; ++i) {
        float complex current = samples[i];
        if (!r->have_previous) {
            r->previous = current; r->have_previous = true;
            if (!emit_iq(r, current)) return false;
            r->output_index = 1;
            continue;
        }
        ++r->input_index;
        while (r->output_index * (uint64_t) SSTV_SAMPLE_RATE <=
               r->input_index * (uint64_t) TX_SAMPLE_RATE) {
            double position = (double) r->output_index * SSTV_SAMPLE_RATE / TX_SAMPLE_RATE;
            float fraction = (float) (position - (double) (r->input_index - 1));
            if (!emit_iq(r, r->previous + fraction * (current - r->previous))) return false;
            ++r->output_index;
        }
        r->previous = current;
    }
    return true;
}

static void *produce(void *argument) {
    producer_t *p = argument;
    resampler_t r = { .ring = p->ring };
    bool ok = sstv_encode_rgb(p->mode, p->pixels, p->stride, resample_iq, &r);
    pthread_mutex_lock(&p->ring->mutex);
    p->ring->producer_done = true;
    p->ring->failed = !ok && !interrupted;
    pthread_cond_broadcast(&p->ring->can_read);
    pthread_cond_broadcast(&p->ring->can_write);
    pthread_mutex_unlock(&p->ring->mutex);
    return NULL;
}

static int transmit(hackrf_transfer *transfer) {
    ring_t *ring = transfer->tx_ctx;
    size_t wanted = (size_t) transfer->buffer_length, copied = 0;
    pthread_mutex_lock(&ring->mutex);
    if (ring->padded_last_block || interrupted || ring->failed) {
        pthread_mutex_unlock(&ring->mutex); return -1;
    }
    while (copied < wanted) {
        while (!ring->used && !ring->producer_done && !ring->failed && !interrupted)
            pthread_cond_wait(&ring->can_read, &ring->mutex);
        if (!ring->used) break;
        size_t part = wanted - copied;
        if (part > ring->used) part = ring->used;
        if (part > RING_SIZE - ring->read_pos) part = RING_SIZE - ring->read_pos;
        memcpy(transfer->buffer + copied, ring->data + ring->read_pos, part);
        ring->read_pos = (ring->read_pos + part) % RING_SIZE;
        ring->used -= part;
        copied += part;
        pthread_cond_signal(&ring->can_write);
    }
    if (copied < wanted) {
        memset(transfer->buffer + copied, 0, wanted - copied);
        ring->padded_last_block = true;
    }
    transfer->valid_length = transfer->buffer_length;
    pthread_mutex_unlock(&ring->mutex);
    return 0;
}

static bool parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !text[0] || !end || *end) return false;
    *value = (uint64_t) parsed;
    return true;
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s --frequency HZ [--power 0..47] [--amp on|off] [--serial ID] MODE INPUT.ppm\n"
                    "Example: %s -f 145500000 --power 20 --amp off M1 image.ppm\n"
                    "--power is the HackRF TX VGA gain in dB (alias: --txvga).\nModes:", program, program);
    for (size_t i = 0; i < sstv_mode_count(); ++i) fprintf(stderr, " %s", sstv_mode_at(i)->short_name);
    fputc('\n', stderr);
}

static bool hackrf_call(int result, const char *operation) {
    if (result == HACKRF_SUCCESS) return true;
    fprintf(stderr, "%s: %s (%d)\n", operation, hackrf_error_name((enum hackrf_error) result), result);
    return false;
}

int main(int argc, char **argv) {
    uint64_t frequency = 0;
    uint32_t txvga = 0;
    bool amp = false;
    const char *serial = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        const char *option = argv[index++];
        if ((!strcmp(option, "-f") || !strcmp(option, "--frequency")) && index < argc) {
            if (!parse_u64(argv[index++], &frequency)) { fprintf(stderr, "Invalid frequency\n"); return 2; }
        } else if ((!strcmp(option, "--power") || !strcmp(option, "--txvga")) && index < argc) {
            uint64_t value;
            if (!parse_u64(argv[index++], &value) || value > 47) {
                fprintf(stderr, "TX VGA gain must be in the range 0..47 dB\n"); return 2;
            }
            txvga = (uint32_t) value;
        } else if (!strcmp(option, "--amp") && index < argc) {
            const char *value = argv[index++];
            if (!strcmp(value, "on")) amp = true;
            else if (!strcmp(value, "off")) amp = false;
            else { fprintf(stderr, "--amp accepts only on or off\n"); return 2; }
        } else if (!strcmp(option, "--serial") && index < argc) serial = argv[index++];
        else if (!strcmp(option, "-h") || !strcmp(option, "--help")) { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (!frequency || argc - index != 2) { usage(argv[0]); return 2; }
    const sstv_mode_t *mode = sstv_mode_find(argv[index]);
    int width = 0, height = 0;
    uint8_t *pixels = mode ? read_ppm(argv[index + 1], &width, &height) : NULL;
    if (!mode || !pixels || width != mode->width || height != mode->transmitted_lines) {
        fprintf(stderr, "Unknown mode, invalid PPM, or dimensions do not match the mode\n");
        free(pixels); return 1;
    }

    ring_t ring = { .mutex = PTHREAD_MUTEX_INITIALIZER, .can_read = PTHREAD_COND_INITIALIZER,
                    .can_write = PTHREAD_COND_INITIALIZER };
    producer_t producer = { mode, pixels, (size_t) width * 3, &ring };
    pthread_t producer_thread;
    if (pthread_create(&producer_thread, NULL, produce, &producer) != 0) {
        fprintf(stderr, "Could not start encoder thread\n"); free(pixels); return 1;
    }
    pthread_mutex_lock(&ring.mutex);
    while (ring.used < RING_SIZE / 2 && !ring.producer_done)
        pthread_cond_wait(&ring.can_read, &ring.mutex);
    pthread_mutex_unlock(&ring.mutex);

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    hackrf_device *device = NULL;
    bool initialized = hackrf_call(hackrf_init(), "hackrf_init");
    bool opened = initialized && hackrf_call(serial ? hackrf_open_by_serial(serial, &device) : hackrf_open(&device), "hackrf_open");
    bool configured = opened &&
        hackrf_call(hackrf_set_sample_rate(device, TX_SAMPLE_RATE), "set sample rate") &&
        hackrf_call(hackrf_set_baseband_filter_bandwidth(device, hackrf_compute_baseband_filter_bw(TX_SAMPLE_RATE)), "set baseband filter") &&
        hackrf_call(hackrf_set_freq(device, frequency), "set frequency") &&
        hackrf_call(hackrf_set_txvga_gain(device, txvga), "set TX VGA gain") &&
        hackrf_call(hackrf_set_amp_enable(device, amp ? 1 : 0), "set RF amplifier");
    bool started = configured && hackrf_call(hackrf_start_tx(device, transmit, &ring), "start TX");
    if (started) {
        printf("Transmitting %s at %llu Hz, TX VGA %u dB, RF amp %s (Ctrl-C to stop)\n",
               mode->short_name, (unsigned long long) frequency, txvga, amp ? "on" : "off");
        while (!interrupted && hackrf_is_streaming(device) == HACKRF_TRUE) usleep(20000);
        hackrf_stop_tx(device);
    }
    pthread_mutex_lock(&ring.mutex);
    if (!started) ring.failed = true;
    pthread_cond_broadcast(&ring.can_read); pthread_cond_broadcast(&ring.can_write);
    pthread_mutex_unlock(&ring.mutex);
    pthread_join(producer_thread, NULL);
    bool failed = ring.failed;
    if (opened) hackrf_close(device);
    if (initialized) hackrf_exit();
    pthread_cond_destroy(&ring.can_read); pthread_cond_destroy(&ring.can_write);
    pthread_mutex_destroy(&ring.mutex); free(pixels);
    if (!started || failed) { fprintf(stderr, "Transmission failed\n"); return 1; }
    return interrupted ? 130 : 0;
}
