#ifndef SSTV_INTERNAL_COLOR_H
#define SSTV_INTERNAL_COLOR_H

#include <stdint.h>

void sstv_robot_to_rgb(uint8_t y, uint8_t ry, uint8_t by, uint8_t *r, uint8_t *g, uint8_t *b);
void sstv_pd_to_rgb(uint8_t y, uint8_t ry, uint8_t by, uint8_t *r, uint8_t *g, uint8_t *b);

#endif
