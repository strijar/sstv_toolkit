# SSTV Toolkit

SSTV Toolkit is a C99 library for streaming SSTV encoding and decoding. It
also provides command-line tools for converting between RGB images and raw IQ
streams.

IQ data uses interleaved little-endian `float32` values in `I, Q` order at a
fixed sample rate of 12800 Hz.

## Features

- Synchronous streaming IQ decoder with line and frame callbacks.
- Streaming RGB888-to-IQ encoder that does not buffer the complete signal.
- Shared mode catalog used by both the encoder and decoder.
- Continuous phase generation and fractional pixel timing.
- Per-line sync recovery to compensate for clock drift.
- Consecutive frame decoding from a single input stream.
- Explicit complete, truncated, and aborted frame events.
- Standalone PPM-to-IQ and IQ-to-PPM command-line tools.
- Direct streaming transmission of a PPM image with HackRF.

Supported modes:

- Martin M1, M2, M3, and M4
- Scottie S1, S2, and DX
- Wraase SC-2 120 and 180
- Pasokon P3, P5, and P7
- Robot 8, 12, and 24 B/W
- Robot 24, 36, and 72 color
- PD50, PD90, PD120, PD160, PD180, PD240, and PD290

## Project layout

- `include/sstv/decoder.h` — public decoder API
- `include/sstv/encoder.h` — public encoder API
- `include/sstv/modes.h` — shared mode catalog
- `include/sstv/iq_file.h` — raw IQ file reader
- `src/` — library implementation
- `tools/sstv_encode.c` — PPM-to-IQ command-line tool
- `tools/sstv_decode.c` — IQ-to-PPM command-line tool
- `tests/` — streaming and encoder/decoder round-trip tests
- `demo/` — example IQ streams and decoded images

## Requirements

- A C99 compiler
- CMake 3.16 or newer
- [liquid-dsp](https://github.com/jgaeddert/liquid-dsp)
- Optional: libhackrf (builds the `sstv_hackrf_tx` utility)

CMake first looks for liquid-dsp through `pkg-config`, then falls back to
searching for its header and library directly.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Install the library, public headers, command-line tools, and CMake package
files with:

```sh
cmake --install build --prefix /usr/local
```

After installation, another CMake project can use the library as follows:

```cmake
find_package(SSTV 1 REQUIRED)
target_link_libraries(my_application PRIVATE SSTV::sstv)
```

## Decoder API

```c
#include <sstv/decoder.h>

static void on_vis(void *user, const sstv_mode_t *mode)
{
    /* A new frame has started. */
}

static void on_line(void *user, int row, int width, const uint8_t *rgb)
{
    /* rgb contains width * 3 RGB888 bytes and is valid only in this call. */
}

static void on_frame(void *user, sstv_frame_status_t status, int rows)
{
    /* status is COMPLETE, TRUNCATED, or ABORTED. */
}

sstv_callbacks_t callbacks = {
    .on_vis = on_vis,
    .on_line = on_line,
    .on_frame = on_frame,
};

sstv_decoder_t *decoder = sstv_decoder_create(&callbacks, user_data);
if (!decoder) {
    /* Initialization failed. */
}

if (!sstv_decoder_push_iq(decoder, samples, sample_count)) {
    /* Processing failed. */
}

sstv_decoder_flush(decoder);
sstv_decoder_destroy(decoder);
```

`sstv_decoder_push_iq()` is synchronous. DSP processing and callbacks run
before it returns. For real-time audio or SDR input, enqueue samples in the
device callback and call the decoder from a worker thread.

A decoder instance must not be called concurrently from multiple threads.
Independent instances do not share mutable processing state and may be used
in separate threads.

`sstv_decoder_flush()` completes the input stream. It never reports a
substantially incomplete line as complete; an unfinished frame produces
`SSTV_FRAME_TRUNCATED`. After a complete or truncated frame, the decoder
returns to VIS search and can receive another frame.

## Encoder API

```c
#include <sstv/encoder.h>

static bool consume_iq(void *user,
                       const float complex *samples,
                       size_t count)
{
    /* Write or transmit this block. Return false to stop encoding. */
    return true;
}

const sstv_mode_t *mode = sstv_mode_find("PD120");
if (!mode) {
    /* Unknown mode. */
}

bool ok = sstv_encode_rgb(
    mode,
    rgb_pixels,
    rgb_stride,
    consume_iq,
    user_data);
```

`sstv_encode_rgb()` accepts an RGB888 image with dimensions
`mode->width × mode->transmitted_lines`. The stride is specified in bytes.
Generated IQ blocks are passed synchronously to the sink callback, so the
complete IQ signal is never held in memory. Returning `false` from the sink
immediately cancels encoding; the sink is not called again and
`sstv_encode_rgb()` returns `false`.

Modes with `line_height > 1`, such as Martin M3/M4 and lower-resolution Robot
B/W modes, expect only the transmitted source rows. The decoder expands them
to `sstv_mode_height(mode)` output rows.

## Mode catalog

The encoder and decoder use the same immutable catalog:

```c
size_t count = sstv_mode_count();

for (size_t i = 0; i < count; ++i) {
    const sstv_mode_t *mode = sstv_mode_at(i);
}

const sstv_mode_t *m1 = sstv_mode_find("M1");
const sstv_mode_t *vis_mode = sstv_mode_from_vis(44);
```

Mode pointers refer to static immutable storage and remain valid for the
lifetime of the process.

## Command-line tools

### Encode PPM to IQ

```sh
build/sstv_encoder M1 input_320x256.ppm output.iq
```

The input must be a binary PPM (`P6`) image with maximum value 255. Its width
and height must match `mode->width` and `mode->transmitted_lines`.

### Decode IQ to PPM

```sh
build/sstv_decoder input.iq output_prefix
```

Complete frames are written as `output_prefix_000.ppm`,
`output_prefix_001.ppm`, and so on. An incomplete I/Q pair is treated as an
input format error. Truncated frames are reported but not written as complete
images.

### Transmit with HackRF

```sh
build/sstv_hackrf_tx --frequency 145500000 --power 20 --amp off M1 input_320x256.ppm
```

`--frequency` is the RF carrier frequency in Hz. `--power` controls the HackRF
TX VGA gain from 0 to 47 dB and defaults to 0 (`--txvga` is an alias). This is
a gain setting, not a calibrated output value in dBm. `--amp` controls the
HackRF RF amplifier and defaults to `off`. Use `--serial ID` to select a device
when more than one HackRF is connected. The image requirements are the same as
for `sstv_encoder`. IQ is linearly resampled to 2 MHz and streamed to the
device; press Ctrl-C to stop early.

Start at low gain and use a suitable antenna, RF filtering, and a frequency on
which you are permitted to transmit.

## Tests

The test suite covers:

- multiple consecutive frames
- truncated input handling
- first-line alignment after VIS
- direct encoder-to-decoder round trips for every supported mode

Run it with:

```sh
ctest --test-dir build --output-on-failure
```

## Limitations

- The IQ sample rate is fixed at 12800 Hz.
- VIS thresholds have primarily been validated with synthetic signals and
  should be tested further with noisy real-world recordings.
- MMSSTV MP/MR/ML and Martin HQ modes are not implemented.
