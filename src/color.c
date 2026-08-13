#include "internal/color.h"

static uint8_t clamp_u8(double value) {
    if (value < 0.0)
        return 0;
    if (value > 255.0)
        return 255;
    return (uint8_t) (value + 0.5);
}

void sstv_robot_to_rgb(uint8_t y_value, uint8_t ry_value, uint8_t by_value, uint8_t *r, uint8_t *g, uint8_t *b) {
    double y = y_value;
    double ry = (double) ry_value - 128.0;
    double by = (double) by_value - 128.0;
    *r = clamp_u8(y + ry);
    *b = clamp_u8(y + by);
    *g = clamp_u8(y - 0.51 * ry - 0.19 * by);
}

void sstv_pd_to_rgb(uint8_t y_value, uint8_t ry_value, uint8_t by_value, uint8_t *r, uint8_t *g, uint8_t *b) {
    double y = (double) y_value - 16.0;
    double ry = (double) ry_value - 128.0;
    double by = (double) by_value - 128.0;
    *r = clamp_u8(0.003906 * (298.082 * y + 408.583 * ry));
    *g = clamp_u8(0.003906 * (298.082 * y - 100.291 * by - 208.120 * ry));
    *b = clamp_u8(0.003906 * (298.082 * y + 516.411 * by));
}
