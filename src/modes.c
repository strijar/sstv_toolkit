#include "sstv/modes.h"

#include <string.h>

#define MODE(name_, short_, sync_, porch_, sep_, pixel_, line_, width_, lines_, height_, enc_, marker_, chroma_porch_, vis_) \
    { name_, short_, sync_, porch_, sep_, pixel_, line_, width_, lines_, height_, enc_, marker_, chroma_porch_, vis_ }

static const sstv_mode_t modes[] = {
    MODE("Martin M1", "M1", 4.862e-3, 0.572e-3, 0.572e-3, 0.4576e-3, 446.446e-3, 320, 256, 1, SSTV_ENC_GBR, 0, 0, 44),
    MODE("Martin M2", "M2", 4.862e-3, 0.572e-3, 0.572e-3, 0.2288e-3, 226.7986e-3, 320, 256, 1, SSTV_ENC_GBR, 0, 0, 40),
    MODE("Martin M3", "M3", 4.862e-3, 0.572e-3, 0.572e-3, 0.4576e-3, 446.446e-3, 320, 128, 2, SSTV_ENC_GBR, 0, 0, 36),
    MODE("Martin M4", "M4", 4.862e-3, 0.572e-3, 0.572e-3, 0.2288e-3, 226.7986e-3, 320, 128, 2, SSTV_ENC_GBR, 0, 0, 32),
    MODE("Scottie S1", "S1", 9e-3, 1.5e-3, 1.5e-3, 0.432e-3, 428.38e-3, 320, 256, 1, SSTV_ENC_GBR, 0, 0, 60),
    MODE("Scottie S2", "S2", 9e-3, 1.5e-3, 1.5e-3, 0.2752e-3, 277.692e-3, 320, 256, 1, SSTV_ENC_GBR, 0, 0, 56),
    MODE("Scottie DX", "SDX", 9e-3, 1.5e-3, 1.5e-3, 1.08053e-3, 1050.3e-3, 320, 256, 1, SSTV_ENC_GBR, 0, 0, 76),
    MODE("Robot 72", "R72", 12e-3, 0, 6e-3, 0.43125e-3, 300e-3, 320, 240, 1, SSTV_ENC_YUV_ROBOT422, 0, 0, 12),
    MODE("Robot 36", "R36", 9e-3, 3e-3, 0, 0.275e-3, 150e-3, 320, 240, 1, SSTV_ENC_YUV_ROBOT, 4.5e-3, 1.5e-3, 8),
    MODE("Robot 24", "R24", 12e-3, 0, 6e-3, 0.275e-3, 200e-3, 320, 120, 1, SSTV_ENC_YUV_ROBOT422, 0, 0, 4),
    MODE("Robot 24 B/W", "R24BW", 7e-3, 0, 0, 0.291e-3, 100e-3, 320, 240, 1, SSTV_ENC_BW, 0, 0, 10),
    MODE("Robot 12 B/W", "R12BW", 7e-3, 0, 0, 0.291e-3, 100e-3, 320, 120, 2, SSTV_ENC_BW, 0, 0, 6),
    MODE("Robot 8 B/W", "R8BW", 7e-3, 0, 0, 0.1871875e-3, 66.9e-3, 320, 120, 2, SSTV_ENC_BW, 0, 0, 2),
    MODE("Wraase SC-2 120", "W2120", 5.5225e-3, 0.5e-3, 0, 0.489039081e-3, 475.530018e-3, 320, 256, 1, SSTV_ENC_RGB, 0, 0, 63),
    MODE("Wraase SC-2 180", "W2180", 5.5225e-3, 0.5e-3, 0, 0.734532e-3, 711.0225e-3, 320, 256, 1, SSTV_ENC_RGB, 0, 0, 55),
    MODE("PD-50", "PD50", 20e-3, 2.08e-3, 0, 0.286e-3, 388.16e-3, 320, 256, 1, SSTV_ENC_YUV_PD, 0, 0, 93),
    MODE("PD-90", "PD90", 20e-3, 2.08e-3, 0, 0.532e-3, 703.04e-3, 320, 256, 1, SSTV_ENC_YUV_PD, 0, 0, 99),
    MODE("PD-120", "PD120", 20e-3, 2.08e-3, 0, 0.19e-3, 508.48e-3, 640, 496, 1, SSTV_ENC_YUV_PD, 0, 0, 95),
    MODE("PD-160", "PD160", 20e-3, 2.08e-3, 0, 0.382e-3, 804.416e-3, 512, 400, 1, SSTV_ENC_YUV_PD, 0, 0, 98),
    MODE("PD-180", "PD180", 20e-3, 2.08e-3, 0, 0.286e-3, 754.24e-3, 640, 496, 1, SSTV_ENC_YUV_PD, 0, 0, 96),
    MODE("PD-240", "PD240", 20e-3, 2.08e-3, 0, 0.382e-3, 1000e-3, 640, 496, 1, SSTV_ENC_YUV_PD, 0, 0, 97),
    MODE("PD-290", "PD290", 20e-3, 2.08e-3, 0, 0.286e-3, 937.28e-3, 800, 616, 1, SSTV_ENC_YUV_PD, 0, 0, 94),
    MODE("Pasokon P3", "P3", 5.208e-3, 1.042e-3, 1.042e-3, 0.2083e-3, 409.375e-3, 640, 496, 1, SSTV_ENC_RGB, 0, 0, 113),
    MODE("Pasokon P5", "P5", 7.813e-3, 1.563e-3, 1.563e-3, 0.3125e-3, 614.065e-3, 640, 496, 1, SSTV_ENC_RGB, 0, 0, 114),
    MODE("Pasokon P7", "P7", 10.417e-3, 2.083e-3, 2.083e-3, 0.4167e-3, 818.747e-3, 640, 496, 1, SSTV_ENC_RGB, 0, 0, 115),
};

size_t sstv_mode_count(void) {
    return sizeof(modes) / sizeof(modes[0]);
}

const sstv_mode_t *sstv_mode_at(size_t index) {
    return index < sstv_mode_count() ? &modes[index] : NULL;
}

const sstv_mode_t *sstv_mode_find(const char *short_name) {
    if (!short_name)
        return NULL;
    for (size_t i = 0; i < sstv_mode_count(); ++i)
        if (strcmp(modes[i].short_name, short_name) == 0)
            return &modes[i];
    return NULL;
}

const sstv_mode_t *sstv_mode_from_vis(int vis_code) {
    for (size_t i = 0; i < sstv_mode_count(); ++i)
        if (modes[i].vis_code == vis_code)
            return &modes[i];
    return NULL;
}

int sstv_mode_height(const sstv_mode_t *mode) {
    return mode ? mode->transmitted_lines * mode->line_height : 0;
}

const char *sstv_encoding_name(sstv_encoding_t encoding) {
    switch (encoding) {
        case SSTV_ENC_GBR:
            return "GBR";
        case SSTV_ENC_RGB:
            return "RGB";
        case SSTV_ENC_YUV_ROBOT:
            return "YUV Robot 4:2:0";
        case SSTV_ENC_YUV_ROBOT422:
            return "YUV Robot 4:2:2";
        case SSTV_ENC_YUV_PD:
            return "YUV PD";
        case SSTV_ENC_BW:
            return "BW";
    }
    return "unknown";
}
