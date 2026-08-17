/* Streaming complex-IQ SSTV decoder at 12800 Hz. The public API lives in
 * sstv/decoder.h; the CLI and file input are implemented separately. */

/* M_PI is not part of ISO C99, so provide a fallback below. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h> /* liquid.h uses va_list without including stdarg.h. */
#include <math.h>
#include <complex.h>
#include <liquid/liquid.h>
#include "sstv/decoder.h"
#include "internal/color.h"
#include "internal/goertzel.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS              SSTV_SAMPLE_RATE
#define BP_LOW_HZ       1000.0
#define BP_HIGH_HZ      2400.0
#define BP_NTAPS        31
#define BP_AS_DB        60.0f
#define FREQDEM_KF      0.1f
#define VIS_BIT_SAMPLES 384
#define VIS_HOP_SAMPLES 32

/* The leader/break scan window must not exceed the shortest element:
 * the 10 ms break (128 samples). A 30 ms VIS-bit window would smear the
 * 1200 Hz break with the adjacent 1900 Hz leader. */
#define VIS_SCAN_SAMPLES             128
#define SSTV_MAX_WIDTH               800
#define SSTV_FLUSH_TOLERANCE_SAMPLES VIS_SCAN_SAMPLES

/* ------------------------------------------------------------------ */
/* Complex band-pass filter implemented with liquid-dsp:              */
/*   down-mix (nco_crcf_mix_down) -> real-coeff LPF (firfilt_crcf)     */
/*   -> up-mix (nco_crcf_mix_up)                                       */
/*                                                                      */
/* firfilt_crcf applies real coefficients to complex input, which is
 * equivalent to filtering the real and imaginary parts independently. */
/* ------------------------------------------------------------------ */

typedef struct {
    nco_crcf     nco;
    firfilt_crcf lpf;
} complex_bandpass_t;

static void cbp_init(complex_bandpass_t *bp, double f_lo, double f_hi, double fs, unsigned int ntaps, float As) {
    double fc = (f_lo + f_hi) / 2.0;
    double half_bw = (f_hi - f_lo) / 2.0;
    float  fc_norm = (float) (half_bw / fs);

    bp->nco = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_frequency(bp->nco, (float) (2.0 * M_PI * fc / fs));

    bp->lpf = firfilt_crcf_create_kaiser(ntaps, fc_norm, As, 0.0f);
    firfilt_crcf_set_scale(bp->lpf, 2.0f * fc_norm);
}

static void cbp_free(complex_bandpass_t *bp) {
    if (bp->nco)
        nco_crcf_destroy(bp->nco);
    if (bp->lpf)
        firfilt_crcf_destroy(bp->lpf);
}

static inline float complex cbp_process(complex_bandpass_t *bp, float complex x) {
    float complex mixed, filtered, out;

    nco_crcf_mix_down(bp->nco, x, &mixed);

    firfilt_crcf_push(bp->lpf, mixed);
    firfilt_crcf_execute(bp->lpf, &filtered);

    nco_crcf_mix_up(bp->nco, filtered, &out);

    /* mix_up/mix_down do not advance the NCO phase. */
    nco_crcf_step(bp->nco);

    return out;
}

/* ------------------------------------------------------------------ */
/* FM discriminator implemented with liquid-dsp freqdem.              */
/* ------------------------------------------------------------------ */

typedef struct {
    freqdem dem;
    float   kf;
} discrim_t;

static void discrim_init(discrim_t *d, float kf) {
    d->kf = kf;
    d->dem = freqdem_create(kf);
}

static void discrim_free(discrim_t *d) {
    if (d->dem)
        freqdem_destroy(d->dem);
}

/* freqdem returns m[k] = arg(conj(r[k-1]) * r[k]) / (2*pi*kf).
 * Convert it to hertz with freq_hz = m * kf * fs. */
static inline double discrim_process(discrim_t *d, float complex x) {
    float m;
    freqdem_demodulate(d->dem, x, &m);
    return (double) m * (double) d->kf * FS;
}

/* ------------------------------------------------------------------ */
/* VIS detector: a state machine driven by Goertzel tone detection.   */
/* ------------------------------------------------------------------ */

typedef enum {
    VIS_SEARCH,
    VIS_LEAD1,
    VIS_BREAK,
    VIS_LEAD2,
    VIS_CAPTURE_BITS,
    VIS_GOT_MODE
} vis_state_t;

#define VIS_TH_TONE           0.35
#define VIS_MIN_LEAD_SAMPLES  ((int) (0.20 * FS))
#define VIS_MAX_BREAK_SAMPLES ((int) (0.05 * FS))

typedef struct {
    float complex ring[VIS_SCAN_SAMPLES]; /* Short leader/break scan window. */
    int           ring_count;
    int           hop_counter;

    vis_state_t state;
    long        state_samples;

    float complex capture[VIS_BIT_SAMPLES * 10]; /* Ten 30 ms VIS blocks. */
    int           capture_pos;

    const sstv_mode_t *detected_mode;
} vis_decoder_t;

static void vis_init(vis_decoder_t *v) {
    memset(v, 0, sizeof(*v));
    v->state = VIS_SEARCH;
}

static void vis_push_ring(vis_decoder_t *v, float complex x) {
    memmove(&v->ring[0], &v->ring[1], (VIS_SCAN_SAMPLES - 1) * sizeof(float complex));
    v->ring[VIS_SCAN_SAMPLES - 1] = x;
    if (v->ring_count < VIS_SCAN_SAMPLES)
        v->ring_count++;
}

/* Windowing delays detection of the second leader's end, so the current
 * sample is already inside the 1200 Hz start bit. Locate the transition
 * in the ring and preserve its tail; otherwise the delay consumes the
 * beginning of the first image line. */
static void vis_begin_capture(vis_decoder_t *v) {
    enum { MIN_START_RUN = 8 };
    int run = 0;
    int boundary = -1;

    for (int i = 1; i < VIS_SCAN_SAMPLES; ++i) {
        float complex phase_step = conjf(v->ring[i - 1]) * v->ring[i];
        double        frequency = cargf(phase_step) * FS / (2.0 * M_PI);
        if (frequency < 1550.0) {
            ++run;
            if (run == MIN_START_RUN) {
                boundary = i - MIN_START_RUN + 1;
                break;
            }
        } else {
            run = 0;
        }
    }

    v->state = VIS_CAPTURE_BITS;
    if (boundary < 0) {
        v->capture_pos = 1;
        v->capture[0] = v->ring[VIS_SCAN_SAMPLES - 1];
        return;
    }

    v->capture_pos = VIS_SCAN_SAMPLES - boundary;
    memcpy(v->capture, v->ring + boundary, (size_t) v->capture_pos * sizeof(v->capture[0]));
}

static int vis_decode_capture(const float complex *cap) {
    int bits[7];
    int parity_bit;

    for (int i = 0; i < 7; i++) {
        const float complex *block = cap + VIS_BIT_SAMPLES * (1 + i);
        double               power0, power1;
        sstv_goertzel_pair_power(block, VIS_BIT_SAMPLES, 1100.0, 1300.0, FS, &power0, &power1, NULL);
        /* VIS uses 1100 Hz for binary one and 1300 Hz for zero. */
        bits[i] = (power0 > power1) ? 1 : 0;
    }
    {
        const float complex *block = cap + VIS_BIT_SAMPLES * 8;
        double               power0, power1;
        sstv_goertzel_pair_power(block, VIS_BIT_SAMPLES, 1100.0, 1300.0, FS, &power0, &power1, NULL);
        parity_bit = (power0 > power1) ? 1 : 0;
    }

    int code = 0, ones = 0;
    for (int i = 0; i < 7; i++) {
        code |= (bits[i] << i);
        ones += bits[i];
    }
    int expected_parity = ones % 2;
    if (expected_parity != parity_bit) {
        fprintf(stderr, "VIS: parity mismatch (code=%d)\n", code);
        return -1;
    }
    return code;
}

static bool vis_process_sample(vis_decoder_t *v, float complex x) {
    if (v->state == VIS_CAPTURE_BITS) {
        v->capture[v->capture_pos++] = x;
        if (v->capture_pos >= VIS_BIT_SAMPLES * 10) {
            int                code = vis_decode_capture(v->capture);
            const sstv_mode_t *mode = sstv_mode_from_vis(code);
            if (mode) {
                v->detected_mode = mode;
                v->state = VIS_GOT_MODE;
                return true;
            } else {
                fprintf(stderr, "VIS: unknown code %d, resuming search\n", code);
                v->state = VIS_SEARCH;
                v->capture_pos = 0;
            }
        }
        return false;
    }

    vis_push_ring(v, x);
    v->hop_counter++;
    if (v->ring_count < VIS_SCAN_SAMPLES || v->hop_counter < VIS_HOP_SAMPLES) {
        return false;
    }
    v->hop_counter = 0;

    double power1900, power1200, signal_energy;
    sstv_goertzel_pair_power(v->ring, VIS_SCAN_SAMPLES, 1900.0, 1200.0, FS, &power1900, &power1200, &signal_energy);
    double threshold_power = VIS_TH_TONE * VIS_TH_TONE *
                             VIS_SCAN_SAMPLES * signal_energy;
    bool tone1900 = power1900 > threshold_power;
    bool tone1200 = power1200 > threshold_power;

    switch (v->state) {
        case VIS_SEARCH:
            if (tone1900) {
                v->state = VIS_LEAD1;
                v->state_samples = VIS_HOP_SAMPLES;
            }
            break;
        case VIS_LEAD1:
            if (tone1900) {
                v->state_samples += VIS_HOP_SAMPLES;
            } else if (tone1200 && v->state_samples >= VIS_MIN_LEAD_SAMPLES) {
                v->state = VIS_BREAK;
                v->state_samples = VIS_HOP_SAMPLES;
            } else {
                v->state = VIS_SEARCH;
            }
            break;
        case VIS_BREAK:
            if (tone1200 && v->state_samples < VIS_MAX_BREAK_SAMPLES) {
                v->state_samples += VIS_HOP_SAMPLES;
            } else if (tone1900) {
                v->state = VIS_LEAD2;
                v->state_samples = VIS_HOP_SAMPLES;
            } else {
                v->state = VIS_SEARCH;
            }
            break;
        case VIS_LEAD2:
            if (tone1900) {
                v->state_samples += VIS_HOP_SAMPLES;
            } else if (v->state_samples >= VIS_MIN_LEAD_SAMPLES) {
                vis_begin_capture(v);
            } else {
                v->state = VIS_SEARCH;
            }
            break;
        default:
            break;
    }

    return false;
}

/* The streaming line decoder operates on a bounded sliding frequency
 * window. Every line is resynchronized on its 1200 Hz pulse. */

/* Frequency-to-level calibration: 1500 Hz is black, 2300 Hz is white. */
static inline uint8_t freq_to_level(double freq_hz) {
    double v = (freq_hz - 1500.0) / 800.0 * 255.0;
    if (v < 0.0)
        v = 0.0;
    if (v > 255.0)
        v = 255.0;
    return (uint8_t) (v + 0.5);
}

/* Finds the 1200 Hz sync pulse near center by maximizing the integrated
 * drop below the 1500 Hz black level over a sync-sized window. */
static int find_sync_start(const double *freq, long n, long center, long radius, int sync_samples) {
    long lo = center - radius;
    long hi = center + radius;
    if (lo < 0)
        lo = 0;
    if (hi + sync_samples > n)
        hi = n - sync_samples;
    if (hi < lo)
        return (int) center; /* No search window; trust the estimate. */

    /* Initial sum at lo. */
    double sum = 0.0;
    for (int i = 0; i < sync_samples; i++)
        sum += (1500.0 - freq[lo + i]);

    double best_sum = sum;
    long   best_pos = lo;

    for (long pos = lo + 1; pos <= hi; pos++) {
        sum += (1500.0 - freq[pos + sync_samples - 1]) - (1500.0 - freq[pos - 1]);
        if (sum > best_sum) {
            best_sum = sum;
            best_pos = pos;
        }
    }
    return (int) best_pos;
}

/* Averages frequency over the fractional interval [start, end). Each
 * sample contributes according to the exact overlap of its unit interval,
 * preventing systematic rounding drift across a scan line. */
static double weighted_average_freq(const double *freq, long n, double start, double end) {
    if (end <= start)
        end = start + 1e-9;

    long i0 = (long) floor(start);
    long i1 = (long) floor(end - 1e-12); /* Last sample touched by the interval. */

    double sum = 0.0;
    double weight_total = 0.0;

    for (long i = i0; i <= i1; i++) {
        if (i < 0 || i >= n)
            continue;
        double lo = (double) i;
        double hi = lo + 1.0;
        double overlap_lo = (lo > start) ? lo : start;
        double overlap_hi = (hi < end) ? hi : end;
        double w = overlap_hi - overlap_lo;
        if (w <= 0.0)
            continue;
        sum += freq[i] * w;
        weight_total += w;
    }

    return (weight_total > 0.0) ? (sum / weight_total) : 1500.0;
}

/* Decodes one to three equal-width channels and returns the fractional
 * position immediately after the last channel. */
static double decode_channels(const double *freq, long n, double line_pos, double pixel_samples, int septr_samples, int width, int n_channels, uint8_t out[3][SSTV_MAX_WIDTH]) {
    double pos = line_pos;
    for (int ch = 0; ch < n_channels; ch++) {
        for (int px = 0; px < width; px++) {
            double seg_start = pos;
            double seg_end = pos + pixel_samples;
            double avg = weighted_average_freq(freq, n, seg_start, seg_end);
            out[ch][px] = freq_to_level(avg);
            pos = seg_end;
        }
        if (ch < n_channels - 1)
            pos += septr_samples;
    }
    return pos;
}

/* VIS establishes the first-line boundary. Later lines are searched near
 * their predicted positions to compensate for clock drift. */
static int resync_line(const double *freq, long n, long expected_start, long search_radius, int sync_samples, bool is_first_line) {
    if (is_first_line)
        return (int) expected_start;
    return find_sync_start(freq, n, expected_start, search_radius, sync_samples);
}

/* ------------------------------------------------------------------ */
/* Streaming decoder state.                                           */
/*                                                                      */
/* IQ is processed into a bounded sliding frequency buffer. Complete
 * lines are emitted through on_line and released while retaining enough
 * history for backward sync searches. After a frame, the state returns
 * to VIS search so consecutive frames can share one input stream.       */
/* ------------------------------------------------------------------ */

struct sstv_decoder {
    agc_crcf           agc;
    complex_bandpass_t bp;
    discrim_t          disc;
    vis_decoder_t      vis;

    const sstv_mode_t *mode; /* NULL until VIS is decoded. */

    cbuffercf raw_cbuf;           /* Raw receiver IQ input. */
    cbufferf  freq_cbuf;          /* Sliding discriminator-frequency window. */
    long      freq_base_abs;      /* Absolute sample index represented by freq_cbuf[0]. */
    long      expected_start_abs; /* Predicted start of the next line or slot. */
    int       next_row;           /* Next output image row. */
    bool      first_line;
    bool      flushing; /* No more input will arrive; decode the final complete line. */

    uint8_t *r36_last_ry, *r36_last_by; /* Robot 36 chroma history. */

    double *scratch; /* Temporary contiguous frequency window. */
    long    scratch_cap;

    /* Per-instance workspace permits independent decoders in different threads. */
    uint8_t channel_vals[3][SSTV_MAX_WIDTH];
    uint8_t y_vals[SSTV_MAX_WIDTH];
    uint8_t y1_vals[SSTV_MAX_WIDTH];
    uint8_t ry_vals[SSTV_MAX_WIDTH / 2];
    uint8_t by_vals[SSTV_MAX_WIDTH / 2];
    uint8_t rgb_row0[SSTV_MAX_WIDTH * 3];
    uint8_t rgb_row1[SSTV_MAX_WIDTH * 3];

    sstv_callbacks_t callbacks;
    void            *user_data;
    bool             failed;
};

/* Reserve two worst-case line/slot durations plus processing headroom. */
static long sstv_worst_case_line_samples(void) {
    double worst = 0.0;
    for (size_t i = 0; i < sstv_mode_count(); i++)
        if (sstv_mode_at(i)->line_time > worst)
            worst = sstv_mode_at(i)->line_time;
    return (long) llround(worst * FS);
}

static void sstv_stream_reset_frame_state(sstv_decoder_t *s) {
    s->mode = NULL;
    s->expected_start_abs = 0;
    s->freq_base_abs = 0;
    s->next_row = 0;
    s->first_line = true;
    free(s->r36_last_ry);
    s->r36_last_ry = NULL;
    free(s->r36_last_by);
    s->r36_last_by = NULL;
    if (s->freq_cbuf)
        cbufferf_reset(s->freq_cbuf);
    vis_init(&s->vis);
}

static bool sstv_stream_init(sstv_decoder_t *s, const sstv_callbacks_t *callbacks, void *user_data) {
    memset(s, 0, sizeof(*s));

    s->agc = agc_crcf_create();
    agc_crcf_set_bandwidth(s->agc, 0.001f);
    cbp_init(&s->bp, BP_LOW_HZ, BP_HIGH_HZ, FS, BP_NTAPS, BP_AS_DB);
    discrim_init(&s->disc, FREQDEM_KF);
    vis_init(&s->vis);

    long cap = 2 * sstv_worst_case_line_samples() + 4096;
    s->raw_cbuf = cbuffercf_create_max((unsigned int) cap, (unsigned int) cap);
    s->freq_cbuf = cbufferf_create_max((unsigned int) cap, (unsigned int) cap);

    if (callbacks)
        s->callbacks = *callbacks;
    s->user_data = user_data;

    sstv_stream_reset_frame_state(s);
    return s->agc && s->bp.nco && s->bp.lpf && s->disc.dem &&
           s->raw_cbuf && s->freq_cbuf;
}

static void sstv_stream_deinit(sstv_decoder_t *s) {
    if (!s)
        return;
    if (s->agc)
        agc_crcf_destroy(s->agc);
    cbp_free(&s->bp);
    discrim_free(&s->disc);
    if (s->raw_cbuf)
        cbuffercf_destroy(s->raw_cbuf);
    if (s->freq_cbuf)
        cbufferf_destroy(s->freq_cbuf);
    free(s->r36_last_ry);
    free(s->r36_last_by);
    free(s->scratch);
}

/* Copies the available frequency window into contiguous double-precision
 * scratch storage. Returns false until the next line can be attempted. */
static bool sstv_prepare_window(sstv_decoder_t *s, long need_samples, long min_need_samples, long *rel_expected, long *avail) {
    long rel = s->expected_start_abs - s->freq_base_abs;
    if (rel < 0)
        rel = 0;

    long have = (long) cbufferf_size(s->freq_cbuf);
    long required = s->flushing ? min_need_samples : need_samples;
    /* Flush still requires enough samples for a complete payload. */
    if (rel + required > have)
        return false;

    unsigned int num_read = 0;
    float       *ptr = NULL;
    cbufferf_read(s->freq_cbuf, (unsigned int) have, &ptr, &num_read);

    if ((long) num_read > s->scratch_cap) {
        double *new_scratch = realloc(s->scratch, sizeof(double) * (size_t) num_read);
        if (!new_scratch) {
            s->failed = true;
            return false;
        }
        s->scratch = new_scratch;
        s->scratch_cap = (long) num_read;
    }
    for (unsigned int i = 0; i < num_read; i++)
        s->scratch[i] = (double) ptr[i];

    *rel_expected = rel;
    *avail = (long) num_read;
    return true;
}

/* Advances the stream and releases history no longer needed for the next
 * backward sync search. */
static void sstv_advance(sstv_decoder_t *s, long new_expected_start_abs, long search_radius) {
    s->expected_start_abs = new_expected_start_abs;
    long keep_from_abs = new_expected_start_abs - search_radius;
    if (keep_from_abs > s->freq_base_abs) {
        long release_n = keep_from_abs - s->freq_base_abs;
        long have = (long) cbufferf_size(s->freq_cbuf);
        if (release_n > have)
            release_n = have;
        cbufferf_release(s->freq_cbuf, (unsigned int) release_n);
        s->freq_base_abs += release_n;
    }
}

static bool sstv_frame_done(sstv_decoder_t *s) {
    return s->next_row >= sstv_mode_height(s->mode);
}

static void sstv_finish_frame(sstv_decoder_t *s) {
    int rows = s->next_row;
    if (s->callbacks.on_frame)
        s->callbacks.on_frame(s->user_data, SSTV_FRAME_COMPLETE, rows);
    sstv_stream_reset_frame_state(s);
}

static bool stream_try_decode_gbr_rgb_bw(sstv_decoder_t *s) {
    const sstv_mode_t *m = s->mode;
    long               line_samples = (long) llround(m->line_time * FS);
    int                sync_samples = (int) llround(m->sync_time * FS);
    int                porch_samples = (int) llround(m->porch_time * FS);
    int                septr_samples = (int) llround(m->separator_time * FS);
    double             pixel_samples = m->pixel_time * FS;
    long               search_radius = line_samples / 10;
    int                width = m->width;
    int                n_channels = (m->encoding == SSTV_ENC_BW) ? 1 : 3;

    long need = sync_samples + search_radius + line_samples;
    long min_need = line_samples - SSTV_FLUSH_TOLERANCE_SAMPLES;
    long rel_expected, avail;
    if (!sstv_prepare_window(s, need, min_need, &rel_expected, &avail))
        return false;

    int sync_start = resync_line(s->scratch, avail, rel_expected, search_radius, sync_samples, s->first_line);
    s->first_line = false;

    double line_pos = (double) sync_start + sync_samples + porch_samples;
    decode_channels(s->scratch, avail, line_pos, pixel_samples, septr_samples, width, n_channels, s->channel_vals);

    for (int px = 0; px < width; px++) {
        uint8_t r, g, b;
        if (m->encoding == SSTV_ENC_BW) {
            r = g = b = s->channel_vals[0][px];
        } else if (m->encoding == SSTV_ENC_GBR) {
            g = s->channel_vals[0][px];
            b = s->channel_vals[1][px];
            r = s->channel_vals[2][px];
        } else {
            r = s->channel_vals[0][px];
            g = s->channel_vals[1][px];
            b = s->channel_vals[2][px];
        }
        s->rgb_row0[px * 3 + 0] = r;
        s->rgb_row0[px * 3 + 1] = g;
        s->rgb_row0[px * 3 + 2] = b;
    }

    for (int rep = 0; rep < m->line_height; rep++) {
        if (s->callbacks.on_line)
            s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row0);
        s->next_row++;
    }

    long new_expected_abs = s->freq_base_abs + sync_start + line_samples;
    sstv_advance(s, new_expected_abs, search_radius);

    if (sstv_frame_done(s))
        sstv_finish_frame(s);
    return true;
}

/* Scottie places the sync pulse between the blue and red scans:
 * separator, green, separator, blue, sync, porch, red.  The sync pulse is
 * therefore the stable timing anchor in the middle of a scan line. */
static bool stream_try_decode_scottie(sstv_decoder_t *s) {
    const sstv_mode_t *m = s->mode;
    int width = m->width;
    long line_samples = (long) llround(m->line_time * FS);
    int sync_samples = (int) llround(m->sync_time * FS);
    int porch_samples = (int) llround(m->porch_time * FS);
    double pixel_samples = m->pixel_time * FS;
    double scan_samples = pixel_samples * width;
    long search_radius = line_samples / 20;

    long before_sync = (long) llround(2.0 * (porch_samples + scan_samples));
    if (s->first_line && s->expected_start_abs == 0)
        s->expected_start_abs = before_sync;

    long need = search_radius + sync_samples + porch_samples + (long) ceil(scan_samples);
    long min_need = sync_samples + porch_samples + (long) ceil(scan_samples) - SSTV_FLUSH_TOLERANCE_SAMPLES;
    long rel_expected, avail;
    if (!sstv_prepare_window(s, need, min_need, &rel_expected, &avail))
        return false;

    int sync_start = find_sync_start(s->scratch, avail, rel_expected, search_radius, sync_samples);
    s->first_line = false;
    double green_pos = (double) sync_start - 2.0 * (porch_samples + scan_samples) + porch_samples;
    double blue_pos = green_pos + scan_samples + porch_samples;
    double red_pos = (double) sync_start + sync_samples + porch_samples;

    for (int px = 0; px < width; ++px) {
        double offset = px * pixel_samples;
        uint8_t g = freq_to_level(weighted_average_freq(s->scratch, avail, green_pos + offset,
                                                         green_pos + offset + pixel_samples));
        uint8_t b = freq_to_level(weighted_average_freq(s->scratch, avail, blue_pos + offset,
                                                         blue_pos + offset + pixel_samples));
        uint8_t r = freq_to_level(weighted_average_freq(s->scratch, avail, red_pos + offset,
                                                         red_pos + offset + pixel_samples));
        s->rgb_row0[px * 3 + 0] = r;
        s->rgb_row0[px * 3 + 1] = g;
        s->rgb_row0[px * 3 + 2] = b;
    }
    if (s->callbacks.on_line)
        s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row0);
    s->next_row++;

    long new_expected_abs = s->freq_base_abs + sync_start + line_samples;
    /* Unlike modes whose payload follows sync, the next Scottie row needs
     * both color scans preceding its sync pulse. */
    sstv_advance(s, new_expected_abs, before_sync + search_radius);
    if (sstv_frame_done(s))
        sstv_finish_frame(s);
    return true;
}

static bool stream_try_decode_pd(sstv_decoder_t *s) {
    const sstv_mode_t *m = s->mode;
    int                width = m->width;
    long               slot_samples = (long) llround(m->line_time * FS);
    int                sync_samples = (int) llround(m->sync_time * FS);
    int                porch_samples = (int) llround(m->porch_time * FS);
    double             pixel_samples = m->pixel_time * FS;
    long               search_radius = slot_samples / 10;

    long need = sync_samples + search_radius + slot_samples;
    long min_need = slot_samples - SSTV_FLUSH_TOLERANCE_SAMPLES;
    long rel_expected, avail;
    if (!sstv_prepare_window(s, need, min_need, &rel_expected, &avail))
        return false;

    int sync_start = resync_line(s->scratch, avail, rel_expected, search_radius, sync_samples, s->first_line);
    s->first_line = false;

    double pos = (double) sync_start + sync_samples + porch_samples;
    double pos_after_3 = decode_channels(s->scratch, avail, pos, pixel_samples, 0, width, 3, s->channel_vals);
    {
        double p = pos_after_3;
        for (int px = 0; px < width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            s->y1_vals[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
    }

    for (int px = 0; px < width; px++) {
        uint8_t r, g, b;
        sstv_pd_to_rgb(s->channel_vals[0][px], s->channel_vals[1][px], s->channel_vals[2][px], &r, &g, &b);
        s->rgb_row0[px * 3 + 0] = r;
        s->rgb_row0[px * 3 + 1] = g;
        s->rgb_row0[px * 3 + 2] = b;
        sstv_pd_to_rgb(s->y1_vals[px], s->channel_vals[1][px], s->channel_vals[2][px], &r, &g, &b);
        s->rgb_row1[px * 3 + 0] = r;
        s->rgb_row1[px * 3 + 1] = g;
        s->rgb_row1[px * 3 + 2] = b;
    }

    if (s->callbacks.on_line)
        s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row0);
    s->next_row++;
    if (s->callbacks.on_line)
        s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row1);
    s->next_row++;

    long new_expected_abs = s->freq_base_abs + sync_start + slot_samples;
    sstv_advance(s, new_expected_abs, search_radius);

    if (sstv_frame_done(s))
        sstv_finish_frame(s);
    return true;
}

static bool stream_try_decode_robot36(sstv_decoder_t *s) {
    const sstv_mode_t *m = s->mode;
    int                width = m->width;
    int                chroma_width = width / 2;
    long               line_samples = (long) llround(m->line_time * FS);
    int                sync_samples = (int) llround(m->sync_time * FS);
    int                porch_samples = (int) llround(m->porch_time * FS);
    int                marker_samples = (int) llround(m->chroma_marker_time * FS);
    int                chroma_porch_samples = (int) llround(m->chroma_porch_time * FS);
    double             pixel_samples = m->pixel_time * FS;
    long               search_radius = line_samples / 10;

    if (!s->r36_last_ry) {
        s->r36_last_ry = malloc((size_t) chroma_width);
        s->r36_last_by = malloc((size_t) chroma_width);
        if (!s->r36_last_ry || !s->r36_last_by) {
            free(s->r36_last_ry);
            s->r36_last_ry = NULL;
            free(s->r36_last_by);
            s->r36_last_by = NULL;
            s->failed = true;
            return false;
        }
        for (int i = 0; i < chroma_width; i++) {
            s->r36_last_ry[i] = 128;
            s->r36_last_by[i] = 128;
        }
    }

    long need = sync_samples + search_radius + line_samples;
    long min_need = line_samples - SSTV_FLUSH_TOLERANCE_SAMPLES;
    long rel_expected, avail;
    if (!sstv_prepare_window(s, need, min_need, &rel_expected, &avail))
        return false;

    int sync_start = resync_line(s->scratch, avail, rel_expected, search_radius, sync_samples, s->first_line);
    s->first_line = false;

    double pos = (double) sync_start + sync_samples + porch_samples;
    {
        double p = pos;
        for (int px = 0; px < width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            s->y_vals[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
        pos = p;
    }

    double marker_freq = weighted_average_freq(s->scratch, avail, pos, pos + marker_samples);
    bool   is_ry = (marker_freq < 1900.0);
    pos += marker_samples;
    pos += chroma_porch_samples;

    uint8_t *target = is_ry ? s->r36_last_ry : s->r36_last_by;
    {
        double p = pos;
        for (int px = 0; px < chroma_width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            target[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
    }

    for (int px = 0; px < width; px++) {
        uint8_t r, g, b;
        int     cx = px / 2;
        sstv_robot_to_rgb(s->y_vals[px], s->r36_last_ry[cx], s->r36_last_by[cx], &r, &g, &b);
        s->rgb_row0[px * 3 + 0] = r;
        s->rgb_row0[px * 3 + 1] = g;
        s->rgb_row0[px * 3 + 2] = b;
    }

    if (s->callbacks.on_line)
        s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row0);
    s->next_row++;

    long new_expected_abs = s->freq_base_abs + sync_start + line_samples;
    sstv_advance(s, new_expected_abs, search_radius);

    if (sstv_frame_done(s))
        sstv_finish_frame(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* Robot 72/24 4:2:2 line layout:
 * sync -> Y(width) -> gap -> R-Y(width/2) -> gap -> B-Y(width/2).
 * Both chroma channels occur on every line, with no porch before Y. */
/* ------------------------------------------------------------------ */

static bool stream_try_decode_robot422(sstv_decoder_t *s) {
    const sstv_mode_t *m = s->mode;
    int                width = m->width;
    int                chroma_width = width / 2;
    long               line_samples = (long) llround(m->line_time * FS);
    int                sync_samples = (int) llround(m->sync_time * FS);
    int                gap_samples = (int) llround(m->separator_time * FS);
    double             pixel_samples = m->pixel_time * FS;
    long               search_radius = line_samples / 10;

    long need = sync_samples + search_radius + line_samples;
    long min_need = line_samples - SSTV_FLUSH_TOLERANCE_SAMPLES;
    long rel_expected, avail;
    if (!sstv_prepare_window(s, need, min_need, &rel_expected, &avail))
        return false;

    int sync_start = resync_line(s->scratch, avail, rel_expected, search_radius, sync_samples, s->first_line);
    s->first_line = false;

    double pos = (double) sync_start + sync_samples; /* Y follows sync directly. */

    {
        double p = pos;
        for (int px = 0; px < width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            s->y_vals[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
        pos = p;
    }
    pos += gap_samples;

    {
        double p = pos;
        for (int px = 0; px < chroma_width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            s->ry_vals[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
        pos = p;
    }
    pos += gap_samples;

    {
        double p = pos;
        for (int px = 0; px < chroma_width; px++) {
            double seg_start = p, seg_end = p + pixel_samples;
            s->by_vals[px] = freq_to_level(weighted_average_freq(s->scratch, avail, seg_start, seg_end));
            p = seg_end;
        }
        pos = p;
    }

    for (int px = 0; px < width; px++) {
        uint8_t r, g, b;
        int     cx = px / 2;
        /* All Robot color modes use the same Y/R-Y/B-Y conversion. */
        sstv_robot_to_rgb(s->y_vals[px], s->ry_vals[cx], s->by_vals[cx], &r, &g, &b);
        s->rgb_row0[px * 3 + 0] = r;
        s->rgb_row0[px * 3 + 1] = g;
        s->rgb_row0[px * 3 + 2] = b;
    }

    if (s->callbacks.on_line)
        s->callbacks.on_line(s->user_data, s->next_row, width, s->rgb_row0);
    s->next_row++;

    long new_expected_abs = s->freq_base_abs + sync_start + line_samples;
    sstv_advance(s, new_expected_abs, search_radius);

    if (sstv_frame_done(s))
        sstv_finish_frame(s);
    return true;
}

static bool stream_try_decode_one(sstv_decoder_t *s) {
    if (!s->mode)
        return false;
    switch (s->mode->encoding) {
        case SSTV_ENC_GBR:
            if (s->mode->vis_code == 60 || s->mode->vis_code == 56 || s->mode->vis_code == 76)
                return stream_try_decode_scottie(s);
            return stream_try_decode_gbr_rgb_bw(s);
        case SSTV_ENC_RGB:
        case SSTV_ENC_BW:
            return stream_try_decode_gbr_rgb_bw(s);
        case SSTV_ENC_YUV_PD:
            return stream_try_decode_pd(s);
        case SSTV_ENC_YUV_ROBOT:
            return stream_try_decode_robot36(s);
        case SSTV_ENC_YUV_ROBOT422:
            return stream_try_decode_robot422(s);
        default:
            return false; /* Unsupported encoding. */
    }
}

/* ------------------------------------------------------------------ */
/* Runs all pending IQ through AGC, band-pass filtering, VIS detection,
 * FM discrimination, and line decoding. */
/* ------------------------------------------------------------------ */

static void sstv_stream_process(sstv_decoder_t *s) {
    unsigned int have = cbuffercf_size(s->raw_cbuf);
    if (have == 0)
        return;

    unsigned int   num_read = 0;
    float complex *ptr = NULL;
    cbuffercf_read(s->raw_cbuf, have, &ptr, &num_read);

    for (unsigned int i = 0; i < num_read; i++) {
        float complex x;
        agc_crcf_execute(s->agc, ptr[i], &x);
        float complex filtered = cbp_process(&s->bp, x);

        if (!s->mode) {
            if (vis_process_sample(&s->vis, filtered)) {
                s->mode = s->vis.detected_mode;
                s->expected_start_abs = 0;
                s->freq_base_abs = 0;
                s->next_row = 0;
                s->first_line = true;
                if (s->callbacks.on_vis)
                    s->callbacks.on_vis(s->user_data, s->mode);
            }
            continue;
        }

        double freq = discrim_process(&s->disc, filtered);

        if (cbufferf_space_available(s->freq_cbuf) == 0) {
            /* Degrade by dropping the oldest sample instead of stalling. */
            float dummy;
            cbufferf_pop(s->freq_cbuf, &dummy);
            s->freq_base_abs++;
            fprintf(stderr, "frequency buffer overflow; decoder is falling behind\n");
        }
        cbufferf_push(s->freq_cbuf, (float) freq);

        while (stream_try_decode_one(s)) { /* Decode every currently complete line. */
        }
    }

    cbuffercf_release(s->raw_cbuf, num_read);
}

/* Synchronous IQ input; callbacks may run before this call returns. */

bool sstv_decoder_push_iq(sstv_decoder_t *s, const float complex *samples, size_t n) {
    if (!s || (!samples && n != 0U) || s->failed)
        return false;
    size_t written = 0;
    while (written < n) {
        unsigned int space = cbuffercf_space_available(s->raw_cbuf);
        if (space == 0) {
            /* Process a full buffer first. Drop its oldest sample only if
             * processing cannot make progress. */
            sstv_stream_process(s);
            space = cbuffercf_space_available(s->raw_cbuf);
            if (space == 0) {
                float complex dummy;
                cbuffercf_pop(s->raw_cbuf, &dummy);
                fprintf(stderr, "raw IQ buffer overflow; decoder is falling behind\n");
                space = 1;
            }
        }
        size_t chunk = n - written;
        if (chunk > space)
            chunk = space;
        cbuffercf_write(s->raw_cbuf, (float complex *) (samples + written), (unsigned int) chunk);
        written += chunk;
    }

    sstv_stream_process(s);
    return !s->failed;
}

/* Finishes the stream. A substantially incomplete frame is truncated. */
void sstv_decoder_flush(sstv_decoder_t *s) {
    if (!s)
        return;
    if (!s->mode && cbuffercf_size(s->raw_cbuf) == 0)
        return;
    sstv_stream_process(s); /* Process any pending raw IQ tail. */
    if (!s->mode)
        return;
    s->flushing = true;
    while (stream_try_decode_one(s)) {
    }
    s->flushing = false;
    if (s->mode) {
        int rows = s->next_row;
        if (s->callbacks.on_frame)
            s->callbacks.on_frame(s->user_data, SSTV_FRAME_TRUNCATED, rows);
        sstv_stream_reset_frame_state(s);
    }
}

sstv_decoder_t *sstv_decoder_create(const sstv_callbacks_t *callbacks, void *user) {
    sstv_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder)
        return NULL;
    if (!sstv_stream_init(decoder, callbacks, user)) {
        sstv_stream_deinit(decoder);
        free(decoder);
        return NULL;
    }
    return decoder;
}

void sstv_decoder_destroy(sstv_decoder_t *decoder) {
    if (!decoder)
        return;
    sstv_stream_deinit(decoder);
    free(decoder);
}

void sstv_decoder_reset(sstv_decoder_t *decoder) {
    if (!decoder)
        return;
    if (decoder->mode && decoder->callbacks.on_frame)
        decoder->callbacks.on_frame(decoder->user_data, SSTV_FRAME_ABORTED, decoder->next_row);
    decoder->failed = false;
    if (decoder->raw_cbuf)
        cbuffercf_reset(decoder->raw_cbuf);
    sstv_stream_reset_frame_state(decoder);
}
